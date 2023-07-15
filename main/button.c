#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "mqtt_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "button.h"
#define BUTTON 0
#define MQTT_TOPIC "button_events"
#define MQTT_BROKER_URI "mqtt://mqtt.innoway.vn:1883"
#define MQTT_PASSWORD "0MWOLFtEBnRcRxKoVFNFb4OkvIkCKMg8"

static const char *TAG = "BUTTON";
static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}
extern __NOINIT_ATTR bool Flag_direct_fota;
extern __NOINIT_ATTR bool Flag_fota;
typedef struct {
	uint8_t pin;
	bool inverted;
	uint16_t history;
	uint32_t down_time;
	uint32_t next_long_time;
} debounce_t;

int pin_count = -1;
debounce_t * debounce;
QueueHandle_t queue;

extern __NOINIT_ATTR bool Flag_quick_pair;
extern __NOINIT_ATTR bool Flag_compatible_pair;



static void update_button(debounce_t *d) {
	d->history = (d->history << 1) | gpio_get_level(d->pin);
}

#define MASK   0b1111000000111111
static bool button_rose(debounce_t *d) {
	if ((d->history & MASK) == 0b0000000000111111) {
		d->history = 0xffff;
		return 1;
	}
	return 0;
}
static bool button_fell(debounce_t *d) {
	if ((d->history & MASK) == 0b1111000000000000) {
		d->history = 0x0000;
		return 1;
	}
	return 0;
}
static bool button_down(debounce_t *d) {
	if (d->inverted) return button_fell(d);
	return button_rose(d);
}
static bool button_up(debounce_t *d) {
	if (d->inverted) return button_rose(d);
	return button_fell(d);
}

static uint32_t millis() {
	return esp_timer_get_time() / 1000;
}

static void send_event(debounce_t db, int ev) {
	button_event_t event = {
			.pin = db.pin,
			.event = ev,
	};
	xQueueSend(queue, &event, portMAX_DELAY);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
        ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}
static void button_debounce_task(void *pvParameter)
{
	for (;;) {
		for (int idx=0; idx<pin_count; idx++) {
			update_button(&debounce[idx]);
			if (button_up(&debounce[idx])) {
				debounce[idx].down_time = 0;
				ESP_LOGI(TAG, "%d UP", debounce[idx].pin);
				send_event(debounce[idx], BUTTON_UP);
			} else if (debounce[idx].down_time && millis() >= debounce[idx].next_long_time) {
				ESP_LOGI(TAG, "%d LONG", debounce[idx].pin);
				debounce[idx].next_long_time = debounce[idx].next_long_time + CONFIG_ESP32_BUTTON_LONG_PRESS_REPEAT_MS;
				send_event(debounce[idx], BUTTON_HELD);
			} else if (button_down(&debounce[idx]) && debounce[idx].down_time == 0) {
				debounce[idx].down_time = millis();
				ESP_LOGI(TAG, "%d DOWN", debounce[idx].pin);
				debounce[idx].next_long_time = debounce[idx].down_time + CONFIG_ESP32_BUTTON_LONG_PRESS_DURATION_MS;
				send_event(debounce[idx], BUTTON_DOWN);
			}
		}
		vTaskDelay(10/portTICK_PERIOD_MS);
	}
}


void button_task(void *arg)
{
    button_event_t ev;
    QueueHandle_t button_events = button_init(PIN_BIT(BUTTON));
    int press_count = 0;
    bool button_pressed = false;
    TickType_t press_start_time = 0;
    TickType_t last_button_press_time = xTaskGetTickCount();

    esp_mqtt_client_handle_t mqtt_client = NULL;
    esp_mqtt_client_config_t mqtt_cfg = {
            .uri = MQTT_BROKER_URI,
	        .password = MQTT_PASSWORD,

    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(mqtt_client);

    while (true)
    {
        if (xQueueReceive(button_events, &ev, 1000 / portTICK_PERIOD_MS))
        {
            if (ev.pin == BUTTON)
            {
                if (ev.event == BUTTON_DOWN)
                {
                    button_pressed = true;
                    press_start_time = xTaskGetTickCount();
                }
                else if (ev.event == BUTTON_UP)
                {
                    if (button_pressed)
                    {
                        button_pressed = false;
                        last_button_press_time = xTaskGetTickCount();
                        TickType_t press_duration = last_button_press_time - press_start_time;
                        if (press_duration >= pdMS_TO_TICKS(1000))
                        {
                            // Nút đã được giữ trong bao nhiêu giây
                            int seconds_held = press_duration / configTICK_RATE_HZ;
                            ESP_LOGI(TAG, "Nut da duoc giu trong %d giay", seconds_held);
                            char payload[32];
                            snprintf(payload, sizeof(payload), "%d", seconds_held);
                            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload, 0, 1, 0);
                        }
                        else
                        {
                            // Nút đã được nhấn
                            press_count++;
                            ESP_LOGI(TAG, "So lan nhan nut: %d", press_count);
                            char payload[32];
                            snprintf(payload, sizeof(payload), "%d", press_count);
                            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload, 0, 1, 0);
                        }
                        // Thời gian từ lúc ấn nút đến lúc thả nút
                        TickType_t press_release_time = last_button_press_time - press_start_time;
                        ESP_LOGI(TAG, "Thoi gian tu luc an nut đen luc tha nut: %d ms", press_release_time * portTICK_PERIOD_MS);
                        char payload[32];
                        snprintf(payload, sizeof(payload), "%d", press_release_time * portTICK_PERIOD_MS);
                        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload, 0, 1, 0);
                    }
                }
            }
        }

        TickType_t current_time = xTaskGetTickCount();
        TickType_t time_since_last_press = current_time - last_button_press_time;
        if (time_since_last_press >= pdMS_TO_TICKS(3000))
        {
            // Reset press count if no button press within 3 seconds
            press_count = 0;
        }
    }
}






QueueHandle_t button_init(unsigned long long pin_select) {
return pulled_button_init(pin_select, GPIO_PULLUP_ONLY);
}

QueueHandle_t pulled_button_init(unsigned long long pin_select, gpio_pull_mode_t pull_mode)
{
if (pin_count != -1) {
	ESP_LOGI(TAG, "Already initialized");
	return NULL;
}

// Configure the pins
gpio_config_t io_conf;
io_conf.intr_type = GPIO_INTR_POSEDGE;
io_conf.mode = GPIO_MODE_INPUT;
io_conf.pull_up_en = (pull_mode == GPIO_PULLUP_ONLY || pull_mode == GPIO_PULLUP_PULLDOWN);
io_conf.pull_down_en = (pull_mode == GPIO_PULLDOWN_ONLY || pull_mode == GPIO_PULLUP_PULLDOWN);;
io_conf.pin_bit_mask = pin_select;
gpio_config(&io_conf);

// Scan the pin map to determine number of pins
pin_count = 0;
for (int pin=0; pin<=39; pin++) {
	if ((1ULL<<pin) & pin_select) {
		pin_count++;
	}
}

// Initialize global state and queue
debounce = calloc(pin_count, sizeof(debounce_t));
queue = xQueueCreate(CONFIG_ESP32_BUTTON_QUEUE_SIZE, sizeof(button_event_t));

// Scan the pin map to determine each pin number, populate the state
uint32_t idx = 0;
for (int pin=0; pin<=39; pin++) {
	if ((1ULL<<pin) & pin_select) {
		ESP_LOGI(TAG, "Registering button input: %d", pin);
		debounce[idx].pin = pin;
					debounce[idx].down_time = 0;
					debounce[idx].inverted = true;
					if (debounce[idx].inverted) debounce[idx].history = 0xffff;
					idx++;
				}
			}

			// Spawn a task to monitor the pins
			xTaskCreate(button_debounce_task, "button_debounce_task", CONFIG_ESP32_BUTTON_TASK_STACK_SIZE, NULL, 200, NULL);
			ESP_LOGI(TAG, "BUTTON initialized");
			return queue;
		}

