/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-21 23:39:37
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-10-22 21:52:32
 * @FilePath: \epd\main\bsp_http.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "bsp_http.h"

static const char *TAG = "BSP_HTTP";
httpd_handle_t server;

/***********************************************************************
 *@author                       foxbrokenleaf 
 *@brief                   事件回调函数
 *@see                          无
 *@param        arg             参数
 *@param        event_base      esp事件
 *@param        event_id        esp事件ID
 *@param        event_data      esp事件数据
 *@return                       无
 *@exception                    无
 *@date                         2026/10/04 20:46
 *@version                      V0.0
 *************************************************************************/
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == ESP_HTTP_SERVER_EVENT) {
        if (event_id == HTTP_SERVER_EVENT_ERROR) {
            esp_https_server_last_error_t *last_error = (esp_tls_last_error_t *) event_data;
            ESP_LOGE(TAG, "Error event triggered: last_error = %s, last_tls_err = %d, tls_flag = %d", esp_err_to_name(last_error->last_error), last_error->esp_tls_error_code, last_error->esp_tls_flags);
        }
        if (event_id == HTTP_SERVER_EVENT_START){
            ESP_LOGI(TAG, "HTTP服务器已启动！");
        }
    }
}

/***********************************************************************
 *@author               foxbrokenleaf
 *@brief           初始化HTTP服务
 *@see                  无
 *@param        server  HTTP服务句柄
 *@return               ESP32事件状态码
 *@exception            停止服务失败
 *@date                 2026/10/04 20:48
 *@version              V0.0
 *************************************************************************/
void Https_Init(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTPS_SERVER_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));


}

/***********************************************************************
 *@author           foxbrokenleaf 
 *@brief       启动Web服务
 *@see              无
 *@param            无
 *@return           HTTP服务句柄
 *@exception        启动服务失败
 *@date             2026/10/04 20:48
 *@version          V0.0
 *************************************************************************/
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = CONFIG_LWIP_MAX_SOCKETS - 6;
    config.max_uri_handlers = 10;

    if (httpd_start(&server, &config) != ESP_OK){
        ESP_LOGI(TAG, "Error starting server!");
        return NULL;        
    }

    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    // httpd_register_uri_handler(server, &XXXXXXXXX);


    return server;
}

/***********************************************************************
 *@author  
 *@brief           停止Web服务
 *@see                  无
 *@param        server  HTTP服务句柄
 *@return               ESP32事件状态码
 *@exception            停止服务失败
 *@date                 2026/10/04 20:48
 *@version              V0.0
 *************************************************************************/
static esp_err_t stop_webserver(httpd_handle_t server)
{
    ESP_LOGI(TAG, "Stopping server");
    return httpd_stop(server);
}

/***********************************************************************
 *@author  
 *@brief                   关闭连接事件
 *@see                          无
 *@param        arg             参数
 *@param        event_base      esp事件
 *@param        event_id        esp事件ID
 *@param        event_data      esp事件数据
 *@return                       无
 *@exception                    HTTP服务对象未创建
 *@date                         2026/10/04 20:48
 *@version                      V0.0
 *************************************************************************/
void disconnect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop https server");
        }
    }
}

/***********************************************************************
 *@author  
 *@brief                   连接事件
 *@see                          无
 *@param        arg             参数
 *@param        event_base      esp事件
 *@param        event_id        esp事件ID
 *@param        event_data      esp事件数据
 *@return                       无
 *@exception                    HTTP服务对象未创建
 *@date                         2026/10/04 20:48
 *@version                      V0.0
 *************************************************************************/
void connect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        *server = start_webserver();
    }
}