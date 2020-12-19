/* GPIO Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include "esp_timer.h"

/**
 * Brief:
 * This test code shows how to configure gpio and how to use gpio interrupt.
 *
 * GPIO status:
 * GPIO18: output
 * GPIO19: output
 * GPIO4:  input, pulled up, interrupt from rising edge and falling edge
 * GPIO5:  input, pulled up, interrupt from rising edge.
 *
 * Test:
 * Connect GPIO18 with GPIO4
 * Connect GPIO19 with GPIO5
 * Generate pulses on GPIO18/19, that triggers interrupt on GPIO4/5
 *
 */

#define GPIO_OUTPUT_IO_0    5 //UEXT pin 10
#define GPIO_OUTPUT_IO_1    2 //UEXT pin 8
#define GPIO_OUTPUT_PIN_SEL  ( (1ULL<<GPIO_OUTPUT_IO_0) | (1ULL <<GPIO_OUTPUT_IO_1) )
#define GPIO_INPUT_IO_0     14 //UEXT pin 9
#define GPIO_INPUT_PIN_SEL  ( 1ULL<<GPIO_INPUT_IO_0 )
#define ESP_INTR_FLAG_DEFAULT 0

static xQueueHandle gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task_example(void* arg)
{
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));
        }
    }
}

static void gpio_set(bool state) {
    gpio_set_level(GPIO_OUTPUT_IO_0, state);
    gpio_set_level(GPIO_OUTPUT_IO_1, state);
}

static void busy_wait(int usec) {
    int64_t timestamp = esp_timer_get_time();
    while (esp_timer_get_time() - timestamp < usec);
}


/* 
Bit formats, from https://tech.jolowe.se/home-automation-rf-protocols/

'1' bit:
 _____
|     |
|     |
|     |_____

|-----|-----|
   T     T

'0' bit:
 _____
|     |
|     |
|     |_________________________

|-----|-------------------------|
   T               5T

'SYNC' bit:
 _____
|     |
|     |
|     |__________________________________________________

|-----|--------------------------------------------------|
   T                         10T

'PAUSE' bit:
 _____
|     |
|     |
|     |_______________________ . . . ____

|-----|----------------------- . . . ----|
   T                40T

T = 250 us
(5T = 1250 us)
(10T = 2500 us)
(40T = 10 ms)
*/

static const int T = 250;   // usec

static void transmit_sync(void) {
    //SYNC
    gpio_set(1);
    busy_wait(T);
    gpio_set(0);
    busy_wait(10*T);
}

static void transmit_phy_bit(uint8_t phy_bit_value) {
    
    gpio_set(1);
    busy_wait(T);
    gpio_set(0);
    if (phy_bit_value) {
        // "1" bit
        busy_wait(T);
    }
    else {
        // "0" bit
        busy_wait(5*T);
    }
}

static void transmit_pause(void) {
    //PAUSE
    gpio_set(1);
    busy_wait(T);
    gpio_set(0);
    busy_wait(40*T);
}

static void transmit_logical_bit(uint8_t bit_value) {
    /* Bitcoding
The data part on the physical link is coded so that every logical bit is sent as two physical bits, where the second one is the inverse of the first one.
'0' => '01'
'1' => '10'
Example: For the logical datastream 0111, is sent over the air as 01101010. */

    if (bit_value) {
        /* Logical bit 1 is to be sent */
        transmit_phy_bit(1);
        transmit_phy_bit(0);
    }
    else {
        transmit_phy_bit(0);
        transmit_phy_bit(1);
    }
}

void app_main()
{
    gpio_config_t io_conf;
    //disable interrupt
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    //interrupt of rising edge
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode    
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    //change gpio intrrupt type for one pin
    gpio_set_intr_type(GPIO_INPUT_IO_0, GPIO_INTR_ANYEDGE);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler, (void*) GPIO_INPUT_IO_0);

    //remove isr handler for gpio number.
    //gpio_isr_handler_remove(GPIO_INPUT_IO_0);

    int cnt = 0;
    while(1) {
        printf("cnt: %d\n", cnt++);


        /* 26 bit station id...
        2^25 (base 10) = 10000000000000000000000000 (base 2) = 0x2000000 (base 16) */
        const uint32_t station_id = 0x200a55a;

        transmit_sync();
        
        /* Transmit 26 bits of station ID */
        for (int i = 0; i < 26; i++) {
            if (station_id & (1 << i)) {
                transmit_logical_bit(1);
            }
            else {
                transmit_logical_bit(0);
            }
        }

        /* Transmit group code (let's try 0) */
        transmit_logical_bit(0);

        /* Transmit the desired state bit */
        transmit_logical_bit(cnt % 2);

        /* Transmit channel bits, '11' for Nexa */
        transmit_logical_bit(1);
        transmit_logical_bit(1);

        /* Transmit unit bits: Nexa Unit #1 = 11, #2 = 10, #3 = 01. */
        transmit_logical_bit(1);
        transmit_logical_bit(1);
        
        transmit_pause();

        vTaskDelay(5000 / portTICK_RATE_MS);
        
    }
}

