/**
 * @file cry_task.h
 * @brief On-device baby-cry classification task.
 *
 * Drains the 1 s audio ring (audio_capture.c), extracts the 29-feature
 * vector (audio_features.c), runs the emlearn forest (cry_model.h) and
 * applies the spec's confirmation rule: confidence >= CRY_CONF_THRESHOLD
 * on at least 2 of the last 3 consecutive 1 s windows. The confirmed flag
 * lands in monitor_state (cry_detected/cry_confidence) and drives the
 * system state machine (cry -> ALERT, with buzzer/LED/LCD via the
 * existing alert machinery).
 */
#ifndef CRY_TASK_H_
#define CRY_TASK_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default confidence (CRY-vote %) above which a 1 s window counts as "cry".
 * Configurable at runtime via the captive portal (default 80%). */
#define CRY_CONF_THRESHOLD 80u

/**
 * No-op by design: the thread is created at build time via
 * K_THREAD_DEFINE() and starts automatically at boot; the explicit call
 * site in main() documents the bring-up order.
 */
void cry_task_start(void);

/** Current cry status threshold percentage (10..99, default 80). */
uint8_t cry_task_get_threshold(void);

/** Update the cry status threshold percentage at runtime (clamped 10..99). */
void cry_task_set_threshold(uint8_t pct);

#ifdef __cplusplus
}
#endif

#endif /* CRY_TASK_H_ */
