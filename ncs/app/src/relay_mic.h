/*
 * SFP-667 (c): the relay's OWN microphone, streamed Opus-compressed to the
 * companion alongside the relayed MouthPad data.
 *
 * Bring-up source is a synthetic sine tone (the real PDM mic is task (b)); the
 * encode + envelope + transport path is identical, so (b) only swaps the sample
 * source. Gated by SetMicrophoneParams{destination=RELAY} from the companion:
 * enable_channel_0 starts/stops, gain is accepted for API parity (unused by the
 * sine source).
 */
#ifndef RELAY_MIC_H
#define RELAY_MIC_H

#include <stdint.h>
#include <stdbool.h>

/* Initialize the Opus encoder. Call once at boot before start/stop. */
void relay_mic_init(void);

/* Start streaming relay_audio_frame messages (~50 fps). gain is stored for
 * parity with the real PDM path; the sine source ignores it. */
void relay_mic_start(uint8_t gain);

/* Stop streaming and reset the encoder state. */
void relay_mic_stop(void);

#endif /* RELAY_MIC_H */
