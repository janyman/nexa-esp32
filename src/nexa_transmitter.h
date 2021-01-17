#include <stdbool.h>

void transmit_init(gpio_num_t _tx_gpio_num);
void transmit_test(void);
void transmit_frame(uint32_t id, bool state);