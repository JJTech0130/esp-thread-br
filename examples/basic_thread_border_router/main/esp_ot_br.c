/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_ot_ota_commands.h"
#include "esp_ot_wifi_cmd.h"
#include "esp_openthread_lock.h"
#include "esp_spiffs.h"
#include "esp_vfs_eventfd.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "border_router_launch.h"
#include "esp_br_web.h"
#include "protocol_examples_common.h"
#include "openthread/dataset.h"

#define TAG "esp_ot_br"

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

static esp_err_t init_spiffs(void)
{
#if CONFIG_AUTO_UPDATE_RCP
    esp_vfs_spiffs_conf_t rcp_fw_conf = {.base_path = "/" CONFIG_RCP_PARTITION_NAME,
                                         .partition_label = CONFIG_RCP_PARTITION_NAME,
                                         .max_files = 10,
                                         .format_if_mount_failed = false};
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&rcp_fw_conf), TAG, "Failed to mount rcp firmware storage");
#endif
#if CONFIG_OPENTHREAD_BR_START_WEB
    esp_vfs_spiffs_conf_t web_server_conf = {
        .base_path = "/spiffs", .partition_label = "web_storage", .max_files = 10, .format_if_mount_failed = false};
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&web_server_conf), TAG, "Failed to mount web storage");
#endif
    return ESP_OK;
}

#if !CONFIG_OPENTHREAD_BR_AUTO_START && CONFIG_EXAMPLE_CONNECT_WIFI
// Without OPENTHREAD_BR_AUTO_START, nothing else in this codebase connects
// Wi-Fi or initializes the border-router backbone automatically - upstream's
// only path for that case is the manual "ot wifi connect" CLI command over
// serial (see esp_ot_cli_extension's esp_ot_process_wifi_cmd). For a headless
// board with no cable after flashing, replicate that same sequence
// (connect -> set backbone netif -> esp_openthread_border_router_init) here
// at boot instead, using the Kconfig-configured SSID/password.
//
// This only brings Wi-Fi and the backbone up - it does NOT touch the Thread
// interface itself. otDatasetGetActiveTlvs()/otIp6SetEnabled()/
// otThreadSetEnabled() are independent of Wi-Fi and aren't re-run
// automatically by the OpenThread stack on reboot, so without the check
// below the device would come back up on Wi-Fi after every power cycle but
// stay stuck at Thread role "disabled" until someone re-PUT /node/state.
// If a dataset is already stored (from an earlier REST API PUT), resume it
// automatically - this is normal border-router behavior (rejoin the network
// you were already on), not the unwanted auto-formation of a brand-new
// network that OPENTHREAD_BR_AUTO_START would do on a truly blank device.
static void connect_wifi_task(void *ctx)
{
    esp_err_t err = esp_ot_wifi_connect(CONFIG_EXAMPLE_WIFI_SSID, CONFIG_EXAMPLE_WIFI_PASSWORD);
    if (err == ESP_OK) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        esp_openthread_set_backbone_netif(get_example_netif());
        ESP_ERROR_CHECK(esp_openthread_border_router_init());

        otOperationalDatasetTlvs dataset;
        if (otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset) == OT_ERROR_NONE) {
            ESP_LOGI(TAG, "Existing Thread dataset found, resuming it");
            ESP_ERROR_CHECK(esp_openthread_auto_start(&dataset));
        } else {
            ESP_LOGI(TAG, "No Thread dataset stored yet, waiting for the REST API");
        }
        esp_openthread_lock_release();

        esp_ot_wifi_border_router_init_flag_set(true);
        ESP_LOGI(TAG, "Connected to Wi-Fi: %s", CONFIG_EXAMPLE_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi: %s", CONFIG_EXAMPLE_WIFI_SSID);
    }
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    // Used eventfds:
    // * netif
    // * task queue
    // * border router
    size_t max_eventfd = 3;

#if CONFIG_OPENTHREAD_RADIO_NATIVE || CONFIG_OPENTHREAD_RADIO_SPINEL_SPI
    // * radio driver (A native radio device needs an eventfd for the radio driver.)
    // * SpiSpinelInterface (The Spi Spinel Interface needs an eventfd.)
    // The above will not exist at the same time.
    max_eventfd++;
#endif
#if CONFIG_OPENTHREAD_RADIO_TREL
    // * TREL reception (The Thread Radio Encapsulation Link needs an eventfd for reception.)
    max_eventfd++;
#endif
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = max_eventfd,
    };

    esp_openthread_config_t openthread_config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config =
            {
                .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
                .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
                .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
            },
    };
    esp_rcp_update_config_t rcp_update_config = ESP_OPENTHREAD_RCP_UPDATE_CONFIG();
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(init_spiffs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if !CONFIG_OPENTHREAD_BR_AUTO_START && CONFIG_EXAMPLE_CONNECT_ETHERNET
// TODO: Add a mechanism for connecting ETH manually.
#error Currently we do not support a manual way to connect ETH, if you want to use ETH, please enable OPENTHREAD_BR_AUTO_START.
#endif

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("esp-ot-br"));
#if CONFIG_OPENTHREAD_CLI_OTA
    esp_set_ota_server_cert((char *)server_cert_pem_start);
#endif

#if CONFIG_OPENTHREAD_BR_START_WEB
    esp_br_web_start("/spiffs");
#endif

    launch_openthread_border_router(&openthread_config, &rcp_update_config);

#if !CONFIG_OPENTHREAD_BR_AUTO_START && CONFIG_EXAMPLE_CONNECT_WIFI
    xTaskCreate(connect_wifi_task, "connect_wifi", 6144, NULL, 4, NULL);
#endif
}
