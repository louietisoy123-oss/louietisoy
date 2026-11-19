/**
 * @file usb_iso_fix.c
 * @brief Recover isochronous endpoints after "incomplete ISO" events.
 *
 * The OTG core arms each isochronous transfer for one specific frame (even or odd).
 * If the transfer misses that frame, the ST HAL disables the endpoint and calls
 * HAL_PCD_ISOOUTIncompleteCallback() / HAL_PCD_ISOINIncompleteCallback(). Zephyr's
 * STM32 UDC driver (4.4) implements neither, so the HAL's empty weak defaults ran and
 * the endpoint stayed disabled for good:
 *   - OUT (audio data): Windows opened the stream and the board received zero bytes.
 *   - IN  (explicit feedback): the host stops seeing feedback, so it never adjusts its
 *     rate to the codec clock.
 *
 * Both overrides re-arm the transfer the HAL had already set up (xfer_buff/xfer_len);
 * the HAL picks the new frame parity when the endpoint is armed again. No Zephyr
 * source is modified.
 */

#include <stdint.h>

#include <stm32f4xx_hal.h>

volatile uint32_t g_iso_out_rearms;
volatile uint32_t g_iso_in_rearms;

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
	PCD_EPTypeDef *ep = &hpcd->OUT_ep[epnum];

	g_iso_out_rearms++;
	(void)HAL_PCD_EP_Receive(hpcd, epnum, ep->xfer_buff, ep->xfer_len);
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
	PCD_EPTypeDef *ep = &hpcd->IN_ep[epnum];

	g_iso_in_rearms++;
	(void)HAL_PCD_EP_Transmit(hpcd, (uint8_t)(epnum | 0x80U), ep->xfer_buff, ep->xfer_len);
}
