/* OTA example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "esp_task_wdt.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "protocol_examples_common.h"
#include "string.h"
#ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "nvs.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include <sys/socket.h>
#if CONFIG_EXAMPLE_CONNECT_WIFI
#include "esp_wifi.h"
#endif

#define HASH_LEN 32
#define BLINK_GPIO 11
static int8_t  FIRMWARE_VERSION = 1;
char rcv_buffer[200];

//NVS lable
#define INT_KEY "int_variable"
#define NVS_NAMESPACE "storage"


//// nvs /////
#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
/* The interface name value can refer to if_desc in esp_netif_defaults.h */
#if CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF_ETH
static const char *bind_interface_name = EXAMPLE_NETIF_DESC_ETH;
#elif CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF_STA
static const char *bind_interface_name = EXAMPLE_NETIF_DESC_STA;
#endif
#endif

static const char *TAG = "simple_ota_example";
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

#define OTA_URL_SIZE 256

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client)) {
				strncpy(rcv_buffer, (char*)evt->data, evt->data_len);
            }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

void write_nvs_variables() {
    nvs_handle_t my_handle;
    esp_err_t err;

    // Initialize NVS
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated or has an old version
        // In this case, erase it and initialize NVS again
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Open NVS handle
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return;
    }

    // Write FIRMWARE_VERSION to NVS
    err = nvs_set_i8(my_handle, INT_KEY, FIRMWARE_VERSION);
    if (err != ESP_OK) {
        printf("Error (%s) writing float to NVS!\n", esp_err_to_name(err));
    }

    // Commit changes
    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) committing changes!\n", esp_err_to_name(err));
    }

    // Close NVS handle
    nvs_close(my_handle);
}

void read_nvs_variables() {
    nvs_handle_t my_handle;
    esp_err_t err;

    // Open NVS handle
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return;
    }
    // Read FIRMWARE_VERSION from NVS
    err = nvs_get_i8(my_handle, INT_KEY, &FIRMWARE_VERSION);
    if (err == ESP_OK) {
        printf("Read float from NVS: %d\n", FIRMWARE_VERSION);
    } else {
        printf("Error (%s) reading float from NVS!\n", esp_err_to_name(err));
    }

    // Close NVS handle
    nvs_close(my_handle);
}


void simple_ota_example_task(void *pvParameter)
{
 while (1) {
    ESP_LOGI(TAG, "Starting OTA over HTTP example...");
    esp_http_client_config_t config = {
        .url = CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL,
        .event_handler = _http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    // downloading the json file
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        // parse the json file	
        cJSON *json = cJSON_Parse(rcv_buffer);
        if (json == NULL) {
            printf("downloaded file is not a valid json, aborting...\n");
        } else {	
            cJSON *version = cJSON_GetObjectItemCaseSensitive(json, "version");
            cJSON *file = cJSON_GetObjectItemCaseSensitive(json, "file");
            
            // check the version
            if (!cJSON_IsNumber(version)) {
                printf("unable to read new version, aborting...\n");
            } else {
                int8_t new_version = version->valuedouble;
                if (new_version > FIRMWARE_VERSION) {
                    printf("current firmware version (%d) is lower than the available one (%d), upgrading...\n", FIRMWARE_VERSION, new_version);
                    if (cJSON_IsString(file) && (file->valuestring != NULL)) {
                        printf("downloading and installing new firmware (%s)...\n", file->valuestring);
                        
                        esp_http_client_config_t ota_client_http_config = {
                            .url = file->valuestring,
                            .cert_pem = (const char *)server_cert_pem_start,
                        };

                        esp_https_ota_config_t ota_client_config = {
                            .http_config = &ota_client_http_config,
                        };

                        ESP_LOGI(TAG, "Attempting to download update from %s", ota_client_http_config.url);
                        esp_err_t ret = esp_https_ota(&ota_client_config);
                        if (ret == ESP_OK) {
                            ESP_LOGI(TAG, "OTA Succeed, Rebooting...");
                            FIRMWARE_VERSION = new_version;
                            write_nvs_variables();
                        } else {
                            ESP_LOGE(TAG, "Firmware upgrade failed");
                        }
                    } else {
                        printf("unable to read the new file name, aborting...\n");
                    }
                } else {
                    printf("current firmware version (%d) is greater or equal to the available one (%d), nothing to do...\n", FIRMWARE_VERSION, new_version);
                }
            }
        }
    } else {
        printf("Checking any version change...\n");
    }

    //cleanup
    esp_http_client_cleanup(client);
    printf("\n");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

}

static void print_sha256(const uint8_t *image_hash, const char *label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i) {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI(TAG, "%s %s", label, hash_print);
}

static void get_sha256_of_partitions(void)
{
    uint8_t sha_256[HASH_LEN] = { 0 };
    esp_partition_t partition;

    // get sha256 digest for bootloader
    partition.address   = ESP_BOOTLOADER_OFFSET;
    partition.size      = ESP_PARTITION_TABLE_OFFSET;
    partition.type      = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader: ");

    // get sha256 digest for running partition
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware: ");
}

// Blink task
void blink_task(void *pvParameter) {
    // start the blink task
	static uint8_t s_led_state = 0;
    gpio_num_t pin_11=11;
    gpio_num_t pin_4=4;
    if(BLINK_GPIO == 4)
    gpio_pulldown_en(pin_11);
    else{
    gpio_pulldown_en(pin_4);   
    }
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    while (1) {
    /* Set the GPIO level according to the state (LOW or HIGH)*/
        gpio_set_level(BLINK_GPIO, s_led_state);
        s_led_state = !s_led_state;
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
}


void app_main(void)
{
    read_nvs_variables();
    ESP_LOGI(TAG, "OTA example app_main start");
    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 1.OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // 2.NVS partition contains data in new format and cannot be recognized by this version of code.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    get_sha256_of_partitions();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

// #if CONFIG_EXAMPLE_CONNECT_WIFI
//     /* Ensure to disable any WiFi power save mode, this allows best throughput
//      * and hence timings for overall OTA operation.
//      */
//     esp_wifi_set_ps(WIFI_PS_NONE);
// #endif // CONFIG_EXAMPLE_CONNECT_WIFI

    xTaskCreate(simple_ota_example_task, "ota_example_task", 16384, NULL, 5, NULL);
    xTaskCreate(blink_task, "blink_task", 16384, NULL, 4, NULL);

}


