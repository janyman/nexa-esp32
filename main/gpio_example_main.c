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

#include "nexa_receiver.h"

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

static xQueueHandle radio_evt_queue = NULL;
static xQueueHandle recv_frame_queue = NULL;

struct nexa_payload {
    uint32_t id : 26;
    bool group : 1;
    bool state : 1;
    uint8_t channel : 2;
    uint8_t unit : 2;
};

static enum nexa_bit_detector_state bit_detector_state = WaitBitStart;
static int64_t bit_detector_timestampLoHi;
static int64_t bit_detector_timestampHiLo;

static bool IRAM_ATTR nexa_allowable_time(int64_t now, int64_t compare_timestamp, int64_t target) {
    int64_t min = target - 25;
    int64_t max = target + 25;

    if ((now - compare_timestamp) < min) {
        return false;
    }
    if ((now - compare_timestamp) > max) {
        return false;
    }
    return true;
}

static const int T = 250;   // usec

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    static bool prev_level;

    uint32_t gpio_num = (uint32_t) arg;
    

    bool level = gpio_get_level(gpio_num);
    int64_t now = esp_timer_get_time();
    enum nexa_condition condition;

    if (level && prev_level) {
        return;
    }

    if (!level && !prev_level) {
        return;
    }

    bool high_to_low, low_to_high;
    if (!prev_level && level) {
        low_to_high = true;
    }
    else {
        low_to_high = false;
    }

    high_to_low = !low_to_high;

    switch (bit_detector_state) {
        case WaitBitStart:
            if (low_to_high) {
                /* low-to-high transition detected */
                bit_detector_state = WaitBitHiLo;
                bit_detector_timestampLoHi = now;
            }
            else {
                condition = PhysicalBitErrorBadEdge1;
                xQueueSendFromISR(radio_evt_queue, &condition, NULL);
            }
            break;
        case WaitBitHiLo:
            if (high_to_low) {
                bit_detector_timestampHiLo = now;
                if (nexa_allowable_time(now, bit_detector_timestampLoHi,T)) {
                    bit_detector_state = WaitBitLoDecision;
                }
                else {
                    /* Bit error */
                    /* Signal physical bit error to anyone listening?? */
                    condition = PhysicalBitErrorBadHighTime;
                    xQueueSendFromISR(radio_evt_queue, &condition, NULL);
                    bit_detector_state = WaitBitStart;
                }
            }
            else {
                condition = PhysicalBitErrorBadEdge2;
                xQueueSendFromISR(radio_evt_queue, &condition, NULL);
                bit_detector_state = WaitBitStart;
            }
            break;
        case WaitBitLoDecision:
            if (low_to_high) {
                if (nexa_allowable_time(now, bit_detector_timestampHiLo, T)) {
                    /* Signal physical "1" bit: MarkConditionDetected */
                    condition = MarkConditionDetected;
                    xQueueSendFromISR(radio_evt_queue, &condition, NULL);
                }
                else if (nexa_allowable_time(now, bit_detector_timestampHiLo, 5*T)) {
                    /* Signal physical "0" bit: SpaceConditionDetected */
                    condition = SpaceConditionDetected;
                    xQueueSendFromISR(radio_evt_queue, &condition, NULL);
                }
                else if (nexa_allowable_time(now, bit_detector_timestampHiLo, 10*T)) {
                    /* Signal sync condition detected */
                    condition = SyncConditionDetected;
                    xQueueSendFromISR(radio_evt_queue, &condition, NULL);
                }
                else if ((now - bit_detector_timestampHiLo) > (40*T-50)) {
                    /* Signal pause conditions detected */
                    condition = PauseConditionDetected;
                    xQueueSendFromISR(radio_evt_queue, &condition, NULL);
                }
                else {
                    condition = PhysicalBitErrorBadLowTime;
                    xQueueSendFromISR(radio_evt_queue, &condition, NULL);
                }
            }
            else {
                condition = PhysicalBitErrorBadEdge3;
                xQueueSendFromISR(radio_evt_queue, &condition, NULL);
            }

            
            /* Any of these situations result in transition to start state */
            bit_detector_timestampLoHi = now;
            bit_detector_state = WaitBitHiLo;
            break;
    }
    prev_level = level;
}

static enum nexa_telegram_detector_state decode_state = WaitSyncCondition;

static void queue_skip_until_sync(void) {
    enum nexa_condition condition;
    while (xQueueReceive(radio_evt_queue, &condition, portMAX_DELAY)) {
        if (condition == SyncConditionDetected) {
            break;
        }
    }
}

static void gpio_task_example(void* arg)
{
    enum nexa_condition condition;
    uint32_t recv_frame = 0;
    int bit_cnt = 0;
    for(;;) {
        if(xQueueReceive(radio_evt_queue, &condition, portMAX_DELAY)) {
            //printf("Nexa condition %i\n", condition);
            switch (decode_state) {
                case WaitSyncCondition:
                    
                    if (condition == SyncConditionDetected) {
                        
                        decode_state = WaitLogicalBitStart;
                        bit_cnt = 0;
                        recv_frame = 0;
                    }
                    else {
                        printf("Unexpected condition %i, was expecting sync\n", condition);
                    }
                    break;
                case WaitLogicalBitStart:
                    
                    if (condition == MarkConditionDetected) {
                        /* Detected start of logical "1" bit. Expect a space condition after that */
                        decode_state = WaitSpaceCondition;
                    }
                    else if (condition == SpaceConditionDetected) {
                        /* Detected start of logical "0" bit. Expect a mark condition after that */
                        decode_state = WaitMarkCondition;
                    }
                    else if (condition == PauseConditionDetected) {
                        printf("End of telegram, recv_frame %x!\n", recv_frame);
                        xQueueSend(recv_frame_queue, &recv_frame, portMAX_DELAY);
                        decode_state = WaitSyncCondition;
                    }
                    else {
                        printf("Unexpected condition %i, was expecting mark, space or pause\n", condition);
                        // Skip queue until sync condition is detected, then go to state WaitLogicalBitStart
                        queue_skip_until_sync();
                        decode_state = WaitLogicalBitStart;
                        bit_cnt = 0;
                        recv_frame = 0;
                    }
                    break;
                case WaitSpaceCondition:
                    if (condition == SpaceConditionDetected) {
                        printf("Logical 1\n");
                        decode_state = WaitLogicalBitStart;
                        recv_frame |= 1 << bit_cnt++;
                    }
                    else {
                        printf("Unexpected condition %i, was expecting %i\n", condition, SpaceConditionDetected);
                        queue_skip_until_sync();
                        decode_state = WaitLogicalBitStart;
                        bit_cnt = 0;
                        recv_frame = 0;
                    }
                    
                    break;
                case WaitMarkCondition: 
                    if (condition == MarkConditionDetected) {
                        printf("Logical 0\n");
                        decode_state = WaitLogicalBitStart;
                        bit_cnt++;
                    }
                    else {
                        printf("Unexpected condition %i, was expecting %i\n", condition, MarkConditionDetected);
                        queue_skip_until_sync();
                        decode_state = WaitLogicalBitStart;
                        bit_cnt = 0;
                        recv_frame = 0;
                    }
                    break;
                case ProtocolError:
                    printf("Protocol error\n");
                    decode_state = WaitSyncCondition;
                    break;
            }

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
    radio_evt_queue = xQueueCreate(100, sizeof(enum nexa_condition));
    recv_frame_queue = xQueueCreate(100, sizeof(uint32_t));
    //start gpio task. Note that this task should be set to *low priority* since GPIO ISR will post, and we cannot allow context switch to happen when isr should be serviced... 
    xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 0, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler, (void*) GPIO_INPUT_IO_0);

    //remove isr handler for gpio number.
    //gpio_isr_handler_remove(GPIO_INPUT_IO_0);

    int cnt = 0;
    while(1) {
        printf("cnt: %d\n", cnt++);

        /* Packetformat
        Every packet consists of a sync bit followed by 26 + 2 + 4 (total 32 logical data part bits) and is ended by a pause bit.

        S HHHH HHHH HHHH HHHH HHHH HHHH HHGO CCEE P

        S = Sync bit.
        H = The first 26 bits are transmitter unique codes, and it is this code that the reciever "learns" to recognize.
        G = Group code. Set to 0 for on, 1 for off.
        O = On/Off bit. Set to 0 for on, 1 for off.
        C = Channel bits. Proove/Anslut = 00, Nexa = 11.
        E = Unit bits. Device to be turned on or off.
        Proove/Anslut Unit #1 = 00, #2 = 01, #3 = 10.
        Nexa Unit #1 = 11, #2 = 10, #3 = 01.
        P = Pause bit.

        For every button press, N identical packets are sent. For Proove/Anslut N is six, and for Nexa it is five.
        */

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

        vTaskDelay(10000 / portTICK_RATE_MS);

        struct nexa_payload frame;
        while (xQueueReceive(recv_frame_queue, &frame, 0)) {
            //printf("Received frame: %x\n", frame);

            //printf("id: %x\n", (frame & 0x3FFFFFF) >> 6);
            printf("id: 0x%x group: %hhi state: %hhi unit %hhi channel %hhi\n", frame.id, frame.group, frame.state, frame.unit, frame.channel);
        }
    }
}

