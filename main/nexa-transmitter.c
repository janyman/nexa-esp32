#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include "esp_timer.h"

#include "nexa_common.h"
#include "nexa_transmitter.h"

static gpio_num_t tx_pin_num;


static void rf_set_transmitter(bool state) {
    gpio_set_level(tx_pin_num, state);
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
    rf_set_transmitter(1);
    busy_wait(T);
    rf_set_transmitter(0);
    busy_wait(10*T);
}

static void transmit_phy_bit(uint8_t phy_bit_value) {
    
    rf_set_transmitter(1);
    busy_wait(T);
    rf_set_transmitter(0);
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
    rf_set_transmitter(1);
    busy_wait(T);
    rf_set_transmitter(0);
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

void transmit_init(gpio_num_t _tx_gpio_num) {

    tx_pin_num = _tx_gpio_num;

    gpio_config_t io_conf;
    //disable interrupt
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = 1ULL<<tx_pin_num;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);
}

void transmit_test(void)
{
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
    for (int i = 0; i < 26; i++)
    {
        if (station_id & (1 << i))
        {
            transmit_logical_bit(1);
        }
        else
        {
            transmit_logical_bit(0);
        }
    }

    /* Transmit group code (let's try 0) */
    transmit_logical_bit(0);

    /* Transmit the desired state bit */
    static int cnt;
    transmit_logical_bit(cnt % 2);

    /* Transmit channel bits, '11' for Nexa */
    transmit_logical_bit(1);
    transmit_logical_bit(1);

    /* Transmit unit bits: Nexa Unit #1 = 11, #2 = 10, #3 = 01. */
    transmit_logical_bit(1);
    transmit_logical_bit(1);

    transmit_pause();
}

void transmit_frame(uint32_t id, bool state)
{
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
    const uint32_t station_id = id;

    transmit_sync();

    /* Transmit 26 bits of station ID */
    for (int i = 0; i < 26; i++)
    {
        if (station_id & (1 << i))
        {
            transmit_logical_bit(1);
        }
        else
        {
            transmit_logical_bit(0);
        }
    }

    /* Transmit group code (let's try 0) */
    transmit_logical_bit(0);

    /* Transmit the desired state bit */
    transmit_logical_bit(state);

    /* Transmit channel bits, '11' for Nexa */
    transmit_logical_bit(1);
    transmit_logical_bit(1);

    /* Transmit unit bits: Nexa Unit #1 = 11, #2 = 10, #3 = 01. */
    transmit_logical_bit(1);
    transmit_logical_bit(1);

    transmit_pause();
}