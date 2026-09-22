/*
 * LumeOS BCM2835 GPIO driver (BCM2835 ARM Peripherals chapter 6).
 */
#ifndef LUME_GPIO_H
#define LUME_GPIO_H

#include <lume/types.h>

#define LUME_GPIO_MAX 54

/* Function selectors from table 6-1 of the datasheet. */
enum gpio_function {
    GPIO_FUNC_INPUT = 0,
    GPIO_FUNC_OUTPUT = 1,
    GPIO_FUNC_ALT0 = 4,
    GPIO_FUNC_ALT1 = 5,
    GPIO_FUNC_ALT2 = 6,
    GPIO_FUNC_ALT3 = 7,
    GPIO_FUNC_ALT4 = 3,
    GPIO_FUNC_ALT5 = 2,
};

enum gpio_pull {
    GPIO_PULL_OFF = 0,
    GPIO_PULL_DOWN = 1,
    GPIO_PULL_UP = 2,
};

void gpio_init(void);
int  gpio_set_function(u32 pin, enum gpio_function func);
int  gpio_set_pull(u32 pin, enum gpio_pull pull);
void gpio_write(u32 pin, int value);
int  gpio_read(u32 pin);
void gpio_set_high(u32 pin);
void gpio_set_low(u32 pin);

#endif /* LUME_GPIO_H */
