#ifndef POWER_LEDS_H
#define POWER_LEDS_H

#include <stdint.h>

int power_leds_init(void);

void power_leds_off(void);
void power_leds_all_on(void);
void power_leds_set(int led1, int led2, int led3, int led4);

void power_leds_show_power_on_progress(int64_t held_ms);
void power_leds_show_power_off_progress(int64_t held_ms);

#endif