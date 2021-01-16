#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include "esp_timer.h"

#include "nexa_common.h"
#include "nexa_receiver.h"



static xQueueHandle radio_evt_queue = NULL;

static bool IRAM_ATTR nexa_allowable_time(int64_t now, int64_t compare_timestamp, int64_t target) {
    int64_t min = target - 150; //FIXME! These limits need to be reviewed! Now a very large "window" is needed
    int64_t max = target + 250;

    if ((now - compare_timestamp) < min) {
        return false;
    }
    if ((now - compare_timestamp) > max) {
        return false;
    }
    return true;
}

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    static enum nexa_bit_detector_state bit_detector_state = WaitBitStart;
    static int64_t bit_detector_timestampLoHi;
    static int64_t bit_detector_timestampHiLo;

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

static void queue_skip_until_sync(void) {
    enum nexa_condition condition;
    while (xQueueReceive(radio_evt_queue, &condition, portMAX_DELAY)) {
        if (condition == SyncConditionDetected) {
            break;
        }
    }
}

static xQueueHandle recv_frame_queue = NULL;

static void radio_event_processor(void* arg)
{
    enum nexa_condition condition;
    enum nexa_telegram_detector_state decode_state = WaitSyncCondition;
    
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
                        //printf("Unexpected condition %i, was expecting sync\n", condition);
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
                        //printf("Unexpected condition %i, was expecting mark, space or pause\n", condition);
                        // Skip queue until sync condition is detected, then go to state WaitLogicalBitStart
                        queue_skip_until_sync();
                        decode_state = WaitLogicalBitStart;
                        bit_cnt = 0;
                        recv_frame = 0;
                    }
                    break;
                case WaitSpaceCondition:
                    if (condition == SpaceConditionDetected) {
                        //printf("Logical 1\n");
                        decode_state = WaitLogicalBitStart;
                        recv_frame |= 1 << bit_cnt++;
                    }
                    else {
                        //printf("Unexpected condition %i, was expecting %i\n", condition, SpaceConditionDetected);
                        queue_skip_until_sync();
                        decode_state = WaitLogicalBitStart;
                        bit_cnt = 0;
                        recv_frame = 0;
                    }
                    
                    break;
                case WaitMarkCondition: 
                    if (condition == MarkConditionDetected) {
                        //printf("Logical 0\n");
                        decode_state = WaitLogicalBitStart;
                        bit_cnt++;
                    }
                    else {
                        //printf("Unexpected condition %i, was expecting %i\n", condition, MarkConditionDetected);
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

#define ESP_INTR_FLAG_DEFAULT 0

void nexa_rx_init(gpio_num_t rx_gpio_num, xQueueHandle _rx_frame_queue) {
    recv_frame_queue = _rx_frame_queue;

    gpio_config_t io_conf;

    //interrupt of rising edge
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = 1ULL << rx_gpio_num;
    //set as input mode    
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    //change gpio intrrupt type for one pin
    gpio_set_intr_type(rx_gpio_num, GPIO_INTR_ANYEDGE);

    //create a queue to handle gpio event from isr
    radio_evt_queue = xQueueCreate(100, sizeof(enum nexa_condition));

    //start gpio task. Note that this task should be set to *low priority* since GPIO ISR will post, and we cannot allow context switch to happen when isr should be serviced... 
    xTaskCreate(radio_event_processor, "gpio_task_example", 2048, NULL, 0, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(rx_gpio_num, gpio_isr_handler, (void*) rx_gpio_num);


}