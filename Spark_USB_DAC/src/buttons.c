#include "buttons.h"
#include <stm32f4xx.h>
#include <zephyr/kernel.h>

void buttons_init(void)
{
    /* Enable GPIOA and GPIOC clocks */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN;

    /* PA0 (CENTER): Input with Pull-Up */
    GPIOA->MODER &= ~(3U << (0 * 2));
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3U << (0 * 2))) | (1U << (0 * 2));

    /* PC0 (LEFT), PC1 (DOWN), PC4 (RIGHT), PC5 (UP): Inputs with Pull-Ups */
    uint32_t pins[] = {0, 1, 4, 5};
    for (int i = 0; i < 4; i++) {
        uint32_t p = pins[i];
        GPIOC->MODER &= ~(3U << (p * 2));
        GPIOC->PUPDR = (GPIOC->PUPDR & ~(3U << (p * 2))) | (1U << (p * 2));
    }
}

button_id_t buttons_poll(void)
{
    static uint32_t last_time = 0;
    static button_id_t last_state = BTN_NONE;

    uint32_t now = k_uptime_get_32();
    if (now - last_time < 30) {
        return BTN_NONE;
    }
    last_time = now;

    button_id_t current = BTN_NONE;
    if (!(GPIOC->IDR & (1U << 5))) {
        current = BTN_UP;
    } else if (!(GPIOC->IDR & (1U << 1))) {
        current = BTN_DOWN;
    } else if (!(GPIOC->IDR & (1U << 0))) {
        current = BTN_LEFT;
    } else if (!(GPIOC->IDR & (1U << 4))) {
        current = BTN_RIGHT;
    } else if (!(GPIOA->IDR & (1U << 0))) {
        current = BTN_CENTER;
    }

    button_id_t pressed = BTN_NONE;
    if (current != BTN_NONE && last_state == BTN_NONE) {
        pressed = current;
    }
    last_state = current;
    return pressed;
}
