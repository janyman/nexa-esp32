/*
 * System Nexa transmitter / receiver test
 * 
 * Connect the radio hardware as follows:
 * 
 * GPIO5 (UEXT pin 10): RF transmitter data pin
 * GPIO14 (UEXT pin 9): RF receiver data pin
 * 
 * For testing, simply use a wire for transmitter->receiver "loop back"
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include "esp_timer.h"

#include "nexa_receiver.h"


void app_main()
{
    /* Setup a queue where any received Nexa command frames will be posted to */
    xQueueHandle recv_frame_queue = xQueueCreate(100, sizeof(uint32_t));

    //UEXT pin 9 is GPIO_NUM_14
    nexa_rx_init(GPIO_NUM_14, recv_frame_queue);
    int cnt = 0;
    while(1) {
        printf("cnt: %d\n", cnt++);

        struct nexa_payload frame;
        while (xQueueReceive(recv_frame_queue, &frame, 10000 / portTICK_RATE_MS)) {
            printf("id: 0x%x group: %hhi state: %hhi unit %hhi channel %hhi\n", frame.id, frame.group, frame.state, frame.unit, frame.channel);
        }
    }
}

