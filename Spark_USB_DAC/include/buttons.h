/**
 * @file buttons.h
 * @brief Debounced 5-way D-Pad controls for RT-Thread Spark Board:
 *        PC5 (UP), PC1 (DOWN), PC0 (LEFT), PC4 (RIGHT), PA0 (CENTER).
 */

#ifndef BUTTONS_H_
#define BUTTONS_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_NONE = 0,
    BTN_UP,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_CENTER
} button_id_t;

void buttons_init(void);
button_id_t buttons_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* BUTTONS_H_ */
