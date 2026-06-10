/*
 * SFP-667 (b)/(c): relay microphone -> Opus -> RelayToAppMessage.relay_audio_frame.
 *
 * Codec config mirrors the MouthPad encoder (mouthpadv1 audio_compression.c):
 * 16 kHz mono, CELT-only, RESTRICTED_LOWDELAY, 16 kbps CBR, complexity 2,
 * fullband -> 320 samples (20 ms) in, ~40 bytes out, so the companion's libopus
 * decoder accepts the frames unchanged.
 *
 * PCM source: the Dotto PDM microphone via the Zephyr DMIC API (nrfx PDM) when
 * the board has a `pdm0` node (CLK P0.05 / DIN P0.26). On boards without it
 * (e.g. the bare DK) it falls back to a synthetic sine tone so the transport
 * path is still exercisable.
 */
#define LOG_MODULE_NAME relay_mic
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <string.h>

#include "opus.h"
#include "opus_defines.h"

#include "MouthpadRelay.pb.h"
#include "usb_cdc.h"
#include "relay_mic.h"

#define MIC_FS              16000
#define MIC_FRAME_SAMPLES   320     /* 20 ms @ 16 kHz */
#define MIC_FRAME_MS        20
#define MIC_BITRATE         16000   /* 16 kbps CBR */
#define MIC_COMPLEXITY      2
#define MIC_MAX_OPUS_BYTES  sizeof(((mouthware_message_AudioDataFrame *)0)->spectral_coeffs_ch0.bytes)

#define HAS_PDM DT_NODE_HAS_STATUS(DT_NODELABEL(pdm0), okay)

#if HAS_PDM
#include <zephyr/audio/dmic.h>
#define PDM_BLOCK_BYTES (MIC_FRAME_SAMPLES * 2)  /* 320 x int16 = 640 B */
#define PDM_BLOCK_COUNT 4
K_MEM_SLAB_DEFINE_STATIC(pdm_slab, PDM_BLOCK_BYTES, PDM_BLOCK_COUNT, 4);
static const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(pdm0));
static bool pdm_configured;
#endif

/* Opus CELT-only encoder state is 7180 bytes; pad for safety and verify at init. */
#define MIC_ENC_BUF_SIZE    8192
static uint8_t __aligned(4) m_enc_buf[MIC_ENC_BUF_SIZE];
static OpusEncoder *const m_enc = (OpusEncoder *)m_enc_buf;
static bool m_enc_ready;

static atomic_t m_active = ATOMIC_INIT(0);
static K_SEM_DEFINE(m_start_sem, 0, 1);
static uint32_t m_frame_index;

#if !HAS_PDM
/* 1 kHz tone at 16 kHz: 16 samples/period, amplitude ~6000 (-14 dBFS).
 * 320 % 16 == 0 so the phase is continuous across frames. */
static const int16_t SINE_1KHZ[16] = {
	0, 2296, 4243, 5543, 6000, 5543, 4243, 2296,
	0, -2296, -4243, -5543, -6000, -5543, -4243, -2296,
};
static uint8_t m_sine_idx;

static void fill_sine(int16_t *pcm, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		pcm[i] = SINE_1KHZ[m_sine_idx];
		m_sine_idx = (m_sine_idx + 1) & 0x0F;
	}
}
#endif

#if HAS_PDM
static int pdm_configure(void)
{
	struct pcm_stream_cfg stream = {
		.pcm_rate = MIC_FS,
		.pcm_width = 16,
		.block_size = PDM_BLOCK_BYTES,
		.mem_slab = &pdm_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			/* Bracket the nRF PDM clocks so the driver lands on ~1.28 MHz
			 * (1.28 MHz / 80 = 16 kHz), matching the MouthPad config. */
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 1300000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = 1,
			.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT),
		},
	};
	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("PDM device not ready");
		return -ENODEV;
	}
	int err = dmic_configure(dmic_dev, &cfg);
	if (err) {
		LOG_ERR("dmic_configure failed (err %d)", err);
		return err;
	}
	pdm_configured = true;
	return 0;
}
#endif

void relay_mic_init(void)
{
	int sz = opus_encoder_get_size(1);
	if (sz <= 0 || sz > (int)sizeof(m_enc_buf)) {
		LOG_ERR("Opus encoder size %d exceeds buffer %d", sz, (int)sizeof(m_enc_buf));
		return;
	}
	if (opus_encoder_init(m_enc, MIC_FS, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY) != OPUS_OK) {
		LOG_ERR("opus_encoder_init failed");
		return;
	}
	opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(MIC_BITRATE));
	opus_encoder_ctl(m_enc, OPUS_SET_VBR(0));
	opus_encoder_ctl(m_enc, OPUS_SET_COMPLEXITY(MIC_COMPLEXITY));
	opus_encoder_ctl(m_enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
	m_enc_ready = true;
#if HAS_PDM
	(void)pdm_configure();
	LOG_INF("Opus encoder ready (state %d bytes), PDM mic source", sz);
#else
	LOG_INF("Opus encoder ready (state %d bytes), sine bring-up source", sz);
#endif
}

void relay_mic_start(uint8_t gain)
{
	if (!m_enc_ready) {
		LOG_WRN("relay mic start ignored: encoder not ready");
		return;
	}
#if HAS_PDM
	if (pdm_configured) {
		int err = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
		if (err) {
			LOG_ERR("dmic_trigger START failed (err %d)", err);
			return;
		}
	}
	LOG_INF("relay mic START (gain %u, PDM source)", gain);
#else
	LOG_INF("relay mic START (gain %u, sine source)", gain);
#endif
	atomic_set(&m_active, 1);
	k_sem_give(&m_start_sem);
}

void relay_mic_stop(void)
{
	if (atomic_set(&m_active, 0) == 1) {
		LOG_INF("relay mic STOP");
#if HAS_PDM
		if (pdm_configured) {
			(void)dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
		}
#else
		m_sine_idx = 0;
#endif
		if (m_enc_ready) {
			opus_encoder_ctl(m_enc, OPUS_RESET_STATE);
		}
	}
}

/* Encode one 320-sample frame and push it to the host as relay_audio_frame. */
static void encode_and_send(const int16_t *pcm)
{
	mouthware_message_RelayToAppMessage msg = mouthware_message_RelayToAppMessage_init_zero;
	msg.which_message_body = mouthware_message_RelayToAppMessage_relay_audio_frame_tag;
	mouthware_message_AudioDataFrame *frame = &msg.message_body.relay_audio_frame;
	frame->index = m_frame_index++;

	opus_int32 n = opus_encode(m_enc, pcm, MIC_FRAME_SAMPLES,
				   frame->spectral_coeffs_ch0.bytes, MIC_MAX_OPUS_BYTES);
	if (n < 0) {
		LOG_ERR("opus_encode failed: %ld", (long)n);
		return;
	}
	frame->spectral_coeffs_ch0.size = (pb_size_t)n;
	(void)usb_cdc_send_proto_message_async(msg);
}

static void mic_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	while (1) {
		if (!atomic_get(&m_active)) {
			k_sem_take(&m_start_sem, K_FOREVER);
			continue;
		}

#if HAS_PDM
		void *block;
		size_t size;
		int err = dmic_read(dmic_dev, 0, &block, &size, 1000);
		if (err) {
			if (atomic_get(&m_active)) {
				LOG_WRN("dmic_read err %d", err);
			}
			continue;  /* re-check active (likely just stopped) */
		}
		encode_and_send((const int16_t *)block);
		k_mem_slab_free(&pdm_slab, block);
		/* dmic_read paces to the 20 ms block; no extra sleep needed. */
#else
		int16_t pcm[MIC_FRAME_SAMPLES];
		int64_t t0 = k_uptime_get();
		fill_sine(pcm, MIC_FRAME_SAMPLES);
		encode_and_send(pcm);
		int64_t dt = k_uptime_get() - t0;
		if (dt < MIC_FRAME_MS) {
			k_sleep(K_MSEC(MIC_FRAME_MS - dt));
		}
#endif
	}
}

/* CELT encode uses large stack VLAs (VAR_ARRAYS + alloca); 8 KB overflowed and
 * corrupted adjacent RAM (BLE GATT tables), faulting in bt_nus_send. 20 KB is
 * comfortably above the measured CELT-complexity-2 peak. */
K_THREAD_DEFINE(relay_mic_tid, 20480, mic_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(7), 0, 0);
