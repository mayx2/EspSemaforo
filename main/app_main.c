#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "mqtt_client.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "esp_sntp.h"

static const char *TAG = "semaforo";
esp_mqtt_client_handle_t global_mqtt_client = NULL;

#define LED_GPIO 21

// Variáveis globais
int tempoCarroPadrao = 0;
int tempoPedestrePadrao = 0;
int tempoCarroPico = 0;
int tempoPedestrePico = 0;
int horarioInicioHora = -1;
int horarioInicioMin = -1;
int horarioFimHora = -1;
int horarioFimMin = -1;

// Configuração dos LEDs
static void configure_leds(void)
{
    gpio_reset_pin(21);
    gpio_set_direction(21, GPIO_MODE_OUTPUT);

    gpio_reset_pin(19);
    gpio_set_direction(19, GPIO_MODE_OUTPUT);

    gpio_reset_pin(18);
    gpio_set_direction(18, GPIO_MODE_OUTPUT);
}

void led_verde(bool on) { gpio_set_level(21, on ? 1 : 0); }
void led_amarelo(bool on) { gpio_set_level(19, on ? 1 : 0); }
void led_vermelho(bool on) { gpio_set_level(18, on ? 1 : 0); }

void init_sntp(void)
{
    ESP_LOGI(TAG, "Inicializando SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}
bool isHorarioPico(struct tm *timeinfo)
{
    int horaAtual = timeinfo->tm_hour;
    int minutoAtual = timeinfo->tm_min;

    int atualMinutos = horaAtual * 60 + minutoAtual;
    int inicioMinutos = horarioInicioHora * 60 + horarioInicioMin;
    int fimMinutos = horarioFimHora * 60 + horarioFimMin;

    ESP_LOGI(TAG, "DEBUG Horario atual: %02d:%02d (%d min)", horaAtual, minutoAtual, atualMinutos);
    ESP_LOGI(TAG, "DEBUG Intervalo pico: inicio=%02d:%02d (%d min) fim=%02d:%02d (%d min)",
             horarioInicioHora, horarioInicioMin, inicioMinutos,
             horarioFimHora, horarioFimMin, fimMinutos);

    // Caso o horário de pico não atravesse a meia-noite
    if (inicioMinutos <= fimMinutos)
    {
        return (atualMinutos >= inicioMinutos && atualMinutos <= fimMinutos);
    }
    // Caso o horário de pico atravesse a meia-noite
    else
    {
        return (atualMinutos >= inicioMinutos || atualMinutos <= fimMinutos);
    }
}


void print_time(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    ESP_LOGI(TAG, "Data/Hora atual: %02d/%02d/%04d %02d:%02d:%02d",
             timeinfo.tm_mday,
             timeinfo.tm_mon + 1,
             timeinfo.tm_year + 1900,
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);
}

// MQTT handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT conectado!");
        esp_mqtt_client_subscribe(client, "Ifpe/Semaforo/Semaforo1", 0);
        break;

    case MQTT_EVENT_DATA:
    {
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        char *payload = strndup(event->data, event->data_len);
        ESP_LOGI(TAG, "Recebido: %s", payload);

        cJSON *json = cJSON_Parse(payload);
        if (json)
        {
            cJSON *tempoPadrao = cJSON_GetObjectItem(json, "tempoPadrao");
            if (tempoPadrao)
            {
                tempoCarroPadrao = cJSON_GetObjectItem(tempoPadrao, "carro")->valueint;
                tempoPedestrePadrao = cJSON_GetObjectItem(tempoPadrao, "pedestre")->valueint;
            }

            cJSON *tempoHorarioPico = cJSON_GetObjectItem(json, "tempoHorarioPico");
            if (tempoHorarioPico)
            {
                tempoCarroPico = cJSON_GetObjectItem(tempoHorarioPico, "carro")->valueint;
                tempoPedestrePico = cJSON_GetObjectItem(tempoHorarioPico, "pedestre")->valueint;
            }

            cJSON *horarioPico = cJSON_GetObjectItem(json, "horarioPico");
            if (horarioPico)
            {
                const char *inicioStr = cJSON_GetObjectItem(horarioPico, "inicio")->valuestring;
                const char *fimStr = cJSON_GetObjectItem(horarioPico, "fim")->valuestring;

                sscanf(inicioStr, "%d:%d", &horarioInicioHora, &horarioInicioMin);
                sscanf(fimStr, "%d:%d", &horarioFimHora, &horarioFimMin);

                ESP_LOGI(TAG, "Horario Pico: inicio %02d:%02d fim %02d:%02d",
                         horarioInicioHora, horarioInicioMin,
                         horarioFimHora, horarioFimMin);
            }

            cJSON_Delete(json);
        }
        free(payload);
        break;
    }

    default:
        break;
    }
}

// Inicializa MQTT
static void mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://broker.hivemq.com:1883"};
    global_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(global_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(global_mqtt_client);
}

static void publicar_estado(const char *estado, int segundos) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "estadoAtual", estado);
    cJSON_AddNumberToObject(root, "segundos", segundos);
    char *json_str = cJSON_PrintUnformatted(root);

    esp_mqtt_client_publish(global_mqtt_client,
        "Ifpe/Semaforo/Semaforo1/estadoAtual",
        json_str, 0, 1, 0);

    cJSON_Delete(root);
    free(json_str);
}

// Task principal do semáforo
static void semaforo_task(void *parameter)
{
    while (1)
    {
        if (tempoCarroPadrao > 0 && tempoPedestrePadrao > 0 &&
            tempoCarroPico > 0 && tempoPedestrePico > 0 &&
            horarioInicioHora >= 0 && horarioFimHora >= 0)
        {

            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);

            bool pico = isHorarioPico(&timeinfo);

            int tempoCarro = pico ? tempoCarroPico : tempoCarroPadrao;
            int tempoPedestre = pico ? tempoPedestrePico : tempoPedestrePadrao;

            ESP_LOGI(TAG, "Modo escolhido: %s", pico ? "HORARIO DE PICO" : "PADRAO");

            // Verde (carros)
            led_verde(true);
            led_amarelo(false);
            led_vermelho(false);
            ESP_LOGI(TAG, "Verde por %d segundos", tempoCarro);
            publicar_estado("verde", tempoCarro);
            vTaskDelay(pdMS_TO_TICKS(tempoCarro * 1000));

            // Amarelo (transição) - 5 segundos
            led_verde(false);
            led_amarelo(true);
            led_vermelho(false);
            ESP_LOGI(TAG, "Amarelo por 5 segundos");
            publicar_estado("amarelo", 5);
            vTaskDelay(pdMS_TO_TICKS(5000));

            // Vermelho (pedestres)
            led_verde(false);
            led_amarelo(false);
            led_vermelho(true);
            ESP_LOGI(TAG, "Vermelho por %d segundos", tempoPedestre);
            publicar_estado("vermelho", tempoPedestre);
            vTaskDelay(pdMS_TO_TICKS(tempoPedestre * 1000));
        }
        else
        {
            ESP_LOGW(TAG, "Ainda não recebeu dados do semáforo via MQTT...");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_sta();
    init_sntp();
    configure_leds();
    mqtt_start();

    // sincronização SNTP
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;
    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count)
    {
        ESP_LOGI(TAG, "Aguardando sincronização do tempo... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    setenv("TZ", "BRT3", 1);
    tzset();

    xTaskCreate(semaforo_task, "SemaforoTask", 4096, NULL, 5, NULL);
}