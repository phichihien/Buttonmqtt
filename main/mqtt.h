/*
 * mqtt.h
 *
 *  Created on: 20 Jun 2022
 *      Author: nguyenphuonglinh
 */

#ifndef MQTT_MQTT_H_
#define MQTT_MQTT_H_

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "esp_wifi_types.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/uart.h"
#include "driver/gpio.h"

void mqtt_app_start(char *broker, char *client_id, char *passowrd);


#endif /* MQTT_MQTT_H_ */
