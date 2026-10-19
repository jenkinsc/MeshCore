// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#define NRF_GPIO_PIN_PULLUP 3
uint32_t nrf_gpio_pin_read(uint32_t pin);
void nrf_gpio_pin_set(uint32_t pin);
void nrf_gpio_pin_clear(uint32_t pin);
void nrf_gpio_pin_write(uint32_t pin, uint32_t v);
void nrf_gpio_cfg_output(uint32_t pin);
void nrf_gpio_cfg_input(uint32_t pin, int pull);
