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
    
    
    xQueueHandle recv_frame_queue = xQueueCreate(100, sizeof(uint32_t));
    int cnt = 0;
    while(1) {
        printf("cnt: %d\n", cnt++);

        
        vTaskDelay(10000 / portTICK_RATE_MS);

        struct nexa_payload frame;
        while (xQueueReceive(recv_frame_queue, &frame, 0)) {
            printf("id: 0x%x group: %hhi state: %hhi unit %hhi channel %hhi\n", frame.id, frame.group, frame.state, frame.unit, frame.channel);
        }
    }
}

