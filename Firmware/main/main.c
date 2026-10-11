#include "lwip/sockets.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "esp_spiffs.h"
#include <sys/stat.h>
#include <esp_http_server.h>
#include "esp_mac.h"
#include "i2cdev.h"
#include "ads111x.h"
#include <lwip/sockets.h>
#include <netinet/tcp.h>

static const char *TAG = "ADC_Osc_Pro_Stable";

// --- CONFIGURACIÓN WI-FI AP ---
#define ESP_WIFI_SSID      "ESP32_OSCILOSCOPIO"
#define ESP_WIFI_PASS      "12345678"
#define MAX_STA_CONN       5

// --- RUTA DE ARCHIVOS EN SPIFFS ---
#define INDEX_PATH      "/spiffs/index.html"
#define UPLOT_JS_PATH   "/spiffs/uPlot.iife.min.js"
#define UPLOT_CSS_PATH  "/spiffs/uPlot.min.css"

// --- CONFIGURACIÓN ADS1115 ---
#define I2C_MASTER_SDA_IO     21
#define I2C_MASTER_SCL_IO     22
#define I2C_MASTER_NUM        I2C_NUM_0
#define I2C_MASTER_FREQ_HZ    400000
#define I2C_ADDRESS           ADS111X_ADDR_GND
#define ALERT_RDY_GPIO        23

// --- PARÁMETROS DE OPTIMIZACIÓN ---
#define BATCH_SIZE              128
#define ADC_QUEUE_LENGTH        512
#define DATA_RATE_FIJO          ADS111X_DATA_RATE_860
#define N_MUESTRAS_PROMEDIO     2

// --- POOL DE BUFFERS ---
#define POOL_SIZE               30

typedef struct {
    int16_t buffer[BATCH_SIZE];
    int count;
    int in_use;
} async_buffer_t;

static async_buffer_t async_pool[POOL_SIZE];
static SemaphoreHandle_t pool_mutex;
static SemaphoreHandle_t buffer_sem;
static QueueHandle_t pending_batch_queue;

// --- VARIABLES GLOBALES ---
static i2c_dev_t ads_dev;
static httpd_handle_t server = NULL;
static SemaphoreHandle_t conversion_ready_sem = NULL;
static QueueHandle_t adc_value_queue = NULL;
static volatile uint32_t isr_counter = 0;
static int active_websocket_clients = 0;
static SemaphoreHandle_t client_count_mutex;

// Variables de Control de Flujo Ping-Pong y Memoria Segura
static volatile bool client_ready_flag = false;
static int global_ws_fd = -1;
static int last_sent_buf_idx = -1; // Guarda el índice del buffer en tránsito

static volatile uint32_t batches_dropped = 0;
static volatile uint32_t batches_sent = 0;
static uint32_t last_batches_dropped = 0;
static uint32_t last_batches_sent = 0;
static volatile TickType_t last_sent_tick = 0;

// --- PROTOTIPOS ---
static void wifi_init_softap(void);
static esp_err_t mount_spiffs(void);
static esp_err_t send_spiffs_file(httpd_req_t *req, const char *filepath, const char *type);
esp_err_t get_index_handler(httpd_req_t *req);
esp_err_t get_uplot_js_handler(httpd_req_t *req);
esp_err_t get_uplot_css_handler(httpd_req_t *req);
static esp_err_t handle_ws_req(httpd_req_t *req);
static esp_err_t start_webserver(void);
void enviar_batch_websocket(int16_t *buffer, int count);
static void ads1115_interrupt_read_task(void *pvParameters);
static void alert_rdy_isr_handler(void* arg);
static esp_err_t setup_alert_rdy_interrupt(void);
static void set_tcp_send_buffer(int sockfd);

// --- GESTIÓN WI-FI ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) ESP_LOGI(TAG, "Cliente conectado");
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) ESP_LOGI(TAG, "Cliente desconectado");
}

void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .channel = 1,
            .password = ESP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_err_t mount_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = { .base_path = "/spiffs", .partition_label = NULL, .max_files = 5, .format_if_mount_failed = true };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "SPIFFS montado exitosamente");
    return ESP_OK;
}

// --- ISR ---
static void IRAM_ATTR alert_rdy_isr_handler(void* arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(conversion_ready_sem, &xHigherPriorityTaskWoken);
    isr_counter++;
    if (xHigherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
}

static esp_err_t setup_alert_rdy_interrupt(void) {
    conversion_ready_sem = xSemaphoreCreateBinary();
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << ALERT_RDY_GPIO),
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(ALERT_RDY_GPIO, alert_rdy_isr_handler, NULL);
    return ESP_OK;
}

// --- POOL DE BUFFERS ---
static int16_t* get_async_buffer(int *idx) {
    if (xSemaphoreTake(buffer_sem, 0) != pdTRUE) return NULL;
    xSemaphoreTake(pool_mutex, portMAX_DELAY);
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!async_pool[i].in_use) {
            async_pool[i].in_use = 1;
            *idx = i;
            xSemaphoreGive(pool_mutex);
            return async_pool[i].buffer;
        }
    }
    xSemaphoreGive(pool_mutex);
    xSemaphoreGive(buffer_sem);
    return NULL;
}

static void release_async_buffer(int idx) {
    if (idx >= 0 && idx < POOL_SIZE) {
        xSemaphoreTake(pool_mutex, portMAX_DELAY);
        async_pool[idx].in_use = 0;
        xSemaphoreGive(pool_mutex);
        xSemaphoreGive(buffer_sem);
    }
}

// --- ENVÍO DE BATCH (solo encola) ---
void enviar_batch_websocket(int16_t *buffer, int count) {
    if (server == NULL || count == 0 || active_websocket_clients == 0) return;
    
    int buf_idx;
    int16_t *payload = get_async_buffer(&buf_idx);
    if (payload == NULL) { batches_dropped++; return; }
    
    memcpy(payload, buffer, count * sizeof(int16_t));
    async_buffer_t *buf_ptr = &async_pool[buf_idx];
    buf_ptr->count = count;
    
    if (uxQueueMessagesWaiting(pending_batch_queue) >= 20) {
        async_buffer_t *old_buf;
        if (xQueueReceive(pending_batch_queue, &old_buf, 0) == pdTRUE) {
            release_async_buffer(old_buf - async_pool);
            batches_dropped++;
        }
    }
    if (xQueueSend(pending_batch_queue, &buf_ptr, 0) != pdTRUE) {
        release_async_buffer(buf_idx);
        batches_dropped++;
    }
}

static void set_tcp_send_buffer(int sockfd) {
    int sndbuf = 65536;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    int flag = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));
}

// --- HANDLERS HTTP ---
static esp_err_t send_spiffs_file(httpd_req_t *req, const char *filepath, const char *type) {
    FILE *f = fopen(filepath, "r");
    if (f == NULL) return httpd_resp_send_404(req);
    httpd_resp_set_type(req, type);
    char chunk[1024]; size_t chunksize;
    while ((chunksize = fread(chunk, 1, sizeof(chunk), f)) > 0) httpd_resp_send_chunk(req, chunk, chunksize);
    fclose(f); httpd_resp_send_chunk(req, NULL, 0); return ESP_OK;
}

esp_err_t get_index_handler(httpd_req_t *req) { return send_spiffs_file(req, INDEX_PATH, "text/html"); }
esp_err_t get_uplot_js_handler(httpd_req_t *req) { return send_spiffs_file(req, UPLOT_JS_PATH, "application/javascript"); }
esp_err_t get_uplot_css_handler(httpd_req_t *req) { return send_spiffs_file(req, UPLOT_CSS_PATH, "text/css"); }

static void ws_free_ctx(void *ctx) {
    int *p = (int *)ctx;
    if (p) {
        xSemaphoreTake(client_count_mutex, portMAX_DELAY);
        active_websocket_clients--;
        if(active_websocket_clients <= 0) {
            active_websocket_clients = 0;
            global_ws_fd = -1; 
            client_ready_flag = false;
            // Limpieza de memoria de red en caso de desconexión abrupta
            if (last_sent_buf_idx != -1) {
                release_async_buffer(last_sent_buf_idx);
                last_sent_buf_idx = -1;
            }
        }
        xSemaphoreGive(client_count_mutex); 
        free(p);
    }
}

static esp_err_t handle_ws_req(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        int sockfd = httpd_req_to_sockfd(req);
        set_tcp_send_buffer(sockfd);
        int *ctx = malloc(sizeof(int)); *ctx = 1;
        httpd_sess_set_ctx(server, sockfd, ctx, ws_free_ctx);
        
        xSemaphoreTake(client_count_mutex, portMAX_DELAY); 
        active_websocket_clients++; 
        xSemaphoreGive(client_count_mutex);
        
        global_ws_fd = sockfd;
        // NO levantamos la bandera aún. Esperamos el evento onopen del JS para iniciar.
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt; uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;
    
    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) return ESP_ERR_NO_MEM;
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        
        if (ret == ESP_OK && strcmp((char*)buf, "ready") == 0) {
            // Confirmación de recepción: ahora es 100% seguro liberar el buffer anterior
            if (last_sent_buf_idx != -1) {
                release_async_buffer(last_sent_buf_idx);
                last_sent_buf_idx = -1;
            }
            client_ready_flag = true; 
        }
        free(buf); 
        if (ret != ESP_OK) return ret;
    }
    
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        httpd_ws_frame_t close_frame; memset(&close_frame, 0, sizeof(httpd_ws_frame_t));
        close_frame.type = HTTPD_WS_TYPE_CLOSE; httpd_ws_send_frame(req, &close_frame);
    }
    return ESP_OK;
}

static esp_err_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 16384; config.max_open_sockets = MAX_STA_CONN + 2;
    config.recv_wait_timeout = 3; config.send_wait_timeout = 3;
    
    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = get_index_handler };
    httpd_uri_t uri_js = { .uri = "/uPlot.iife.min.js", .method = HTTP_GET, .handler = get_uplot_js_handler };
    httpd_uri_t uri_css = { .uri = "/uPlot.min.css", .method = HTTP_GET, .handler = get_uplot_css_handler };
    httpd_uri_t ws = { .uri = "/ws", .method = HTTP_GET, .handler = handle_ws_req, .is_websocket = true };
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_index);
        httpd_register_uri_handler(server, &uri_js);
        httpd_register_uri_handler(server, &uri_css);
        httpd_register_uri_handler(server, &ws);
        
        TaskHandle_t httpd_task = xTaskGetHandle("httpd");
        if (httpd_task != NULL) vTaskPrioritySet(httpd_task, 5);
        return ESP_OK;
    }
    return ESP_FAIL;
}

// --- TAREA ADC ---
static void ads1115_interrupt_read_task(void *pvParameters) {
    int32_t acumulador = 0;
    int muestras_acumuladas = 0;
    
    while (1) {
        if (xSemaphoreTake(conversion_ready_sem, pdMS_TO_TICKS(100)) == pdTRUE) {
            int16_t raw_val;
            if (ads111x_get_value(&ads_dev, &raw_val) == ESP_OK) {
                acumulador += raw_val;
                muestras_acumuladas++;
                
                if (muestras_acumuladas >= N_MUESTRAS_PROMEDIO) {
                    int16_t promedio = (int16_t)(acumulador / muestras_acumuladas);
                    if (xQueueSend(adc_value_queue, &promedio, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "Cola ADC llena, muestra perdida");
                    }
                    acumulador = 0; muestras_acumuladas = 0;
                }
            }
        }
    }
}

// --- MAIN ---
void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init()); mount_spiffs(); wifi_init_softap(); vTaskDelay(pdMS_TO_TICKS(500));
    
    pool_mutex = xSemaphoreCreateMutex(); client_count_mutex = xSemaphoreCreateMutex();
    buffer_sem = xSemaphoreCreateCounting(POOL_SIZE, POOL_SIZE);
    pending_batch_queue = xQueueCreate(20, sizeof(async_buffer_t*));
    
    start_webserver();
    
    ESP_ERROR_CHECK(i2cdev_init()); memset(&ads_dev, 0, sizeof(i2c_dev_t));
    if (ads111x_init_desc(&ads_dev, I2C_ADDRESS, I2C_MASTER_NUM, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO) == ESP_OK) {
        ads111x_set_comp_high_thresh(&ads_dev, 0x8000); ads111x_set_comp_low_thresh(&ads_dev, 0x0000);
        ads111x_set_gain(&ads_dev, ADS111X_GAIN_2V048); ads111x_set_input_mux(&ads_dev, ADS111X_MUX_0_GND);
        ads111x_set_mode(&ads_dev, ADS111X_MODE_CONTINUOUS); ads111x_set_data_rate(&ads_dev, DATA_RATE_FIJO);
        ads111x_set_comp_queue(&ads_dev, ADS111X_COMP_QUEUE_1);
        setup_alert_rdy_interrupt();
    }
    
    adc_value_queue = xQueueCreate(ADC_QUEUE_LENGTH, sizeof(int16_t));
    xTaskCreatePinnedToCore(ads1115_interrupt_read_task, "ADS_Task", 4096, NULL, 24, NULL, 1);
    
    int16_t batch_buffer[BATCH_SIZE]; int batch_count = 0; int16_t dato_recibido;
    TickType_t last_log_time = xTaskGetTickCount(); uint32_t last_isr_count = 0;
    
    last_sent_tick = xTaskGetTickCount();
    
    while (1) {
        // Recepción y empaquetado del ADC
        if (xQueueReceive(adc_value_queue, &dato_recibido, pdMS_TO_TICKS(10)) == pdTRUE) {
            batch_buffer[batch_count++] = dato_recibido;
            if (batch_count >= BATCH_SIZE) {
                enviar_batch_websocket(batch_buffer, batch_count);
                batch_count = 0;
            }
        }
        
        // Despacho asíncrono validado por la confirmación Ping-Pong del frontend
        if (client_ready_flag && active_websocket_clients > 0 && global_ws_fd != -1) {
            async_buffer_t *buf_ptr;
            if (xQueueReceive(pending_batch_queue, &buf_ptr, 0) == pdTRUE) {
                client_ready_flag = false; // Bloqueamos envíos hasta el próximo 'ready'
                
                httpd_ws_frame_t ws_pkt;
                memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                ws_pkt.payload = (uint8_t *)buf_ptr->buffer;
                ws_pkt.len = buf_ptr->count * sizeof(int16_t);
                ws_pkt.type = HTTPD_WS_TYPE_BINARY;

                // Envío asíncrono correcto (3 argumentos)
                esp_err_t err = httpd_ws_send_frame_async(server, global_ws_fd, &ws_pkt);
                
                if (err == ESP_OK) {
                    batches_sent++;
                    last_sent_tick = xTaskGetTickCount();
                    // Reservamos el índice para liberarlo cuando llegue el próximo "ready"
                    last_sent_buf_idx = buf_ptr - async_pool; 
                } else {
                    batches_dropped++;
                    // Si falló instantáneamente, lo podemos liberar aquí
                    release_async_buffer(buf_ptr - async_pool);
                }
            }
        }
        
        TickType_t now = xTaskGetTickCount();
        if ((now - last_log_time) * portTICK_PERIOD_MS >= 5000) {
            uint32_t current_isr = isr_counter;
            ESP_LOGI(TAG, "Heap: %" PRIu32 " | WS: %d | ADC/5s: %" PRIu32 " | Batches: %" PRIu32 "/%" PRIu32, 
                     esp_get_free_heap_size(), active_websocket_clients, (current_isr - last_isr_count), 
                     (batches_sent - last_batches_sent), (batches_dropped - last_batches_dropped));
            last_isr_count = current_isr; last_batches_dropped = batches_dropped;
            last_batches_sent = batches_sent; last_log_time = now;
        }
    }
}