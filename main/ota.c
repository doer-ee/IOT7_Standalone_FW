#include "main.h"
#include "ota.h"

const char *TAG  = "OTA";

char updata_url[]="http://xxx.xxx.xxx/ota.bin";
int percentage=0; //OTA progress: 0 means failure, 1-100 is progress

void ota_task(void *pvParameter)
{

    uint8_t evt=0;
    int bin_len=0;
    int download_len=0;
    int percentage_old=0;


    esp_err_t ota_finish_err = ESP_OK;
    esp_http_client_config_t config = {
        .url = updata_url,
        .cert_pem = NULL,
        .timeout_ms = 60000,
    };


    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "ESP HTTPS OTA Begin failed");
        percentage=0;
        evt=WIFINET_OTA_PRO;
        xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
        vTaskDelete(NULL);
    }

    esp_app_desc_t app_desc;
    err = esp_https_ota_get_img_desc(https_ota_handle, &app_desc);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "esp_https_ota_read_img_desc failed");
        percentage=0;
        evt=WIFINET_OTA_PRO;
        xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
        goto ota_end;
    }
   bin_len=esp_https_ota_get_image_size(https_ota_handle);
   ESP_LOGI(TAG, " --------- Image bytes : %d",bin_len );
    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        // esp_https_ota_perform returns after every read operation which gives user the ability to
        // monitor the status of OTA upgrade by calling esp_https_ota_get_image_len_read, which gives length of image
        // data read so far.
        download_len=esp_https_ota_get_image_len_read(https_ota_handle);
        //ESP_LOGI(TAG, "Image bytes read: %d",download_len );

        percentage=(float)download_len/bin_len*100;
        if(percentage_old!=percentage)
        {
            percentage_old=percentage;
            ESP_LOGI(TAG, "percentage: %d",percentage );
            evt=WIFINET_OTA_PRO;
            xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
        }  
    }

    if (esp_https_ota_is_complete_data_received(https_ota_handle) != true) {
        // the OTA image was not completely received and user can customise the response to this situation.
        ESP_LOGI(TAG, "Complete data was not received.");
        percentage=0;
        evt=WIFINET_OTA_PRO;
        xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
    }

ota_end:
    ota_finish_err = esp_https_ota_finish(https_ota_handle);
    if ((err == ESP_OK) && (ota_finish_err == ESP_OK)) {
        ESP_LOGI(TAG, "ESP_HTTPS_OTA upgrade successful. Rebooting ...");
        percentage=100;
        evt=WIFINET_OTA_PRO;
        xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
        ESP_LOGI(TAG, "percentage: %d",percentage );
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        }
        ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed %d", ota_finish_err);
        percentage=0;
        evt=WIFINET_OTA_PRO;
        xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
        vTaskDelete(NULL); 
    }
}