#ifndef STUB_HARDWARE_GPIO_H
#define STUB_HARDWARE_GPIO_H

#include <stdint.h>
#include <stdbool.h>

typedef unsigned int uint;

#define GPIO_IN 0
#define GPIO_OUT 1

void gpio_init(uint pin);
void gpio_set_dir(uint pin, bool out);
void gpio_pull_up(uint pin);
void gpio_put(uint pin, bool value);
bool gpio_get(uint pin);

#endif
