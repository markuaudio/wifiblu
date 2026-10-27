#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"
#include "esp_hf_client_api.h"
#include "esp_hf_client_legacy_api.h"
#include "board_config.h"
#include "es8388.h"
#include "audio_io.h"
#include "bt_core.h"
#include "power_guard.h"
#include "audio_engine.h"

static const char *TAG = "audio";
static unsigned boot_profile = AUDIO_BOOT_DAC_PROFILE; // after NVS / self-test / power-trip adjustment
static bool cal_pending;   // run the power self-test at the next audio task iteration
static void power_self_test(void);
static bool pa_flag_nvs;   // mirror of NVS audio/paon: 1 while the amplifier is on

// RTC memory does not survive the IMP809 EN-pin reset on this board (verified
// 2026-10-27: five resets, flag never seen), so the "amplifier was on" flag
// lives in NVS. Written only on change: a couple of writes per listening session.
static void save_pa_flag(bool on)
{
    if (pa_flag_nvs == on) return;
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "paon", on ? 1 : 0); nvs_commit(h); nvs_close(h);
    pa_flag_nvs = on;
}

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static RingbufHandle_t music_ring, call_rx_ring, call_tx_ring;

typedef enum { SRC_MUSIC, SRC_CALL } source_t;
static struct {
    unsigned epoch, music_rate, music_channels, music_volume, music_queued;
    unsigned call_rate, call_volume, dac_profile, test_mode;
    source_t source;
    bool music_active, ring, diagnose;
    uint32_t music_drops, music_underruns, call_rx_drops, call_tx_drops, call_underruns, i2s_errors;
} st = {.music_rate = 44100, .music_channels = 2, .music_volume = MUSIC_BOOT_VOLUME,
        .call_rate = 8000, .call_volume = CALL_BOOT_VOLUME, .dac_profile = AUDIO_BOOT_DAC_PROFILE};
typedef struct { unsigned epoch, channels; uint32_t length; uint8_t data[]; } packet_t;

static const int16_t sine32[] = {
    0,1598,3135,4551,5793,6811,7568,8035,8192,8035,7568,6811,5793,4551,3135,1598,
    0,-1598,-3135,-4551,-5793,-6811,-7568,-8035,-8192,-8035,-7568,-6811,-5793,-4551,-3135,-1598
};

#define BUMP() do { portENTER_CRITICAL(&lock); ++st.epoch; portEXIT_CRITICAL(&lock); } while (0)

// ---------------------------------------------------------------- music API
void audio_engine_music_format(unsigned rate, unsigned channels)
{
    portENTER_CRITICAL(&lock);
    st.music_rate = rate; st.music_channels = channels; ++st.epoch;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "Music PCM %u Hz, %u channel(s)", rate, channels);
}
void audio_engine_music_stream(bool active)
{
    // Idempotent: a repeated "stopped" must not reclock the bus again.
    portENTER_CRITICAL(&lock);
    if (st.music_active != active) { st.music_active = active; ++st.epoch; }
    portEXIT_CRITICAL(&lock);
}
void audio_engine_music_set_volume(unsigned percent)
{
    if (percent > 100) percent = 100;
    portENTER_CRITICAL(&lock); st.music_volume = percent; portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "Music volume %u%%", percent);
}
unsigned audio_engine_music_get_volume(void)
{
    portENTER_CRITICAL(&lock); unsigned v = st.music_volume; portEXIT_CRITICAL(&lock);
    return v;
}
void audio_engine_music_push(const uint8_t *data, uint32_t length)
{
    unsigned epoch, channels; bool active; source_t source;
    portENTER_CRITICAL(&lock);
    active = st.music_active; epoch = st.epoch; channels = st.music_channels; source = st.source;
    portEXIT_CRITICAL(&lock);
    if (!active || source != SRC_MUSIC || !length || length % (channels * sizeof(int16_t))) return;
    packet_t *p;
    if (xRingbufferSendAcquire(music_ring, (void **)&p, sizeof(*p) + length, 0) != pdTRUE) {
        portENTER_CRITICAL(&lock); ++st.music_drops; portEXIT_CRITICAL(&lock); return;
    }
    p->epoch = epoch; p->channels = channels; p->length = length;
    memcpy(p->data, data, length);
    portENTER_CRITICAL(&lock); st.music_queued += length; portEXIT_CRITICAL(&lock);
    xRingbufferSendComplete(music_ring, p);
}


// ----------------------------------------------------------------- call API
void audio_engine_call_start(unsigned rate)
{
    portENTER_CRITICAL(&lock);
    st.source = SRC_CALL; st.call_rate = rate; st.ring = false; ++st.epoch;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "CALL audio start: %u Hz mono both ways", rate);
}
void audio_engine_call_stop(void)
{
    portENTER_CRITICAL(&lock);
    bool was_call = st.source == SRC_CALL;
    if (was_call) { st.source = SRC_MUSIC; ++st.epoch; }
    portEXIT_CRITICAL(&lock);
    if (was_call) ESP_LOGI(TAG, "CALL audio stop; back to music path");
}
bool audio_engine_call_active(void)
{
    portENTER_CRITICAL(&lock); bool a = st.source == SRC_CALL; portEXIT_CRITICAL(&lock);
    return a;
}
// Byte rings hand data back in two pieces at the wrap point. Read exactly
// `want` bytes (possibly in two steps) or nothing at all, so callers never see
// a short read that only looks like an underrun.
static size_t ring_read_exact(RingbufHandle_t ring, size_t capacity, void *dst, size_t want)
{
    if (capacity - xRingbufferGetCurFreeSize(ring) < want) return 0;
    size_t done = 0;
    while (done < want) {
        size_t got = 0;
        uint8_t *data = xRingbufferReceiveUpTo(ring, &got, 0, want - done);
        if (!data) break;
        memcpy((uint8_t *)dst + done, data, got);
        vRingbufferReturnItem(ring, data);
        done += got;
    }
    return done;
}

void audio_engine_call_push(const uint8_t *pcm, uint32_t length)
{
    // Bluetooth callback (phone voice). Byte ring: partial frames are fine.
    if (!length) return;
    if (xRingbufferSend(call_rx_ring, pcm, length, 0) != pdTRUE) {
        portENTER_CRITICAL(&lock); ++st.call_rx_drops; portEXIT_CRITICAL(&lock);
    }
}
uint32_t audio_engine_call_pull(uint8_t *pcm, uint32_t length)
{
    // Bluetooth asks for exactly `length` bytes of microphone PCM; the stack
    // discards partial reads, so hand out full chunks only.
    if (length > CALL_TX_BUFFER_BYTES) return 0;
    return ring_read_exact(call_tx_ring, CALL_TX_BUFFER_BYTES, pcm, length);
}
void audio_engine_call_set_volume(unsigned level)
{
    if (level > 15) level = 15;
    portENTER_CRITICAL(&lock); st.call_volume = level; portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "Call speaker volume %u/15", level);
}
unsigned audio_engine_call_get_volume(void)
{
    portENTER_CRITICAL(&lock); unsigned v = st.call_volume; portEXIT_CRITICAL(&lock);
    return v;
}
void audio_engine_ring(bool on)
{
    portENTER_CRITICAL(&lock);
    bool changed = st.ring != on; st.ring = on; if (changed) ++st.epoch;
    portEXIT_CRITICAL(&lock);
    if (changed) ESP_LOGI(TAG, "Local ring tone %s", on ? "ON" : "OFF");
}

// ----------------------------------------------------------- diagnostics API
void audio_engine_test_mode(unsigned mode)
{
    if (mode > 3) return;
    portENTER_CRITICAL(&lock); st.test_mode = mode; ++st.epoch; portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "Test mode %u: 0=Bluetooth, 1=left 400 Hz, 2=right 800 Hz, 3=silence", mode);
}
void audio_engine_dac_step(int delta)
{
    portENTER_CRITICAL(&lock); int p = (int)st.dac_profile; portEXIT_CRITICAL(&lock);
    p += delta;
    if (p < 0) p = 0;
    if (p > (int)es8388_dac_profile_max()) p = es8388_dac_profile_max();
    portENTER_CRITICAL(&lock); st.dac_profile = p; ++st.epoch; portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "DAC profile P%d requested; reboot returns to P%u", p, boot_profile);
}
void audio_engine_dac_reset(void)
{
    // Forget the self-test result and the learned limit; the self-test runs again now.
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "dac"); nvs_erase_key(h, "cal"); nvs_erase_key(h, "calstep"); nvs_commit(h); nvs_close(h);
    }
    boot_profile = AUDIO_BOOT_DAC_PROFILE;
    portENTER_CRITICAL(&lock); st.dac_profile = AUDIO_BOOT_DAC_PROFILE; cal_pending = true; ++st.epoch; portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "DAC profile reset to P%u and saved limit cleared; running the power self-test again", (unsigned)AUDIO_BOOT_DAC_PROFILE);
}
void audio_engine_calibrate(void)
{
    portENTER_CRITICAL(&lock); cal_pending = true; ++st.epoch; portEXIT_CRITICAL(&lock);
}
void audio_engine_log_status(void)
{
    portENTER_CRITICAL(&lock); st.diagnose = true; portEXIT_CRITICAL(&lock);
}

// ------------------------------------------------------------- DSP helpers
// One limiter gain for both channels keeps the stereo image; fast attack
// prevents overflow, ~150 ms release avoids pumping.
static bool scale_frame(int32_t left, int32_t right, float gain, float max_gain,
                        float *ceiling, float release_step, int16_t *out)
{
    uint32_t peak = left < 0 ? -left : left;
    uint32_t right_peak = right < 0 ? -right : right;
    if (right_peak > peak) peak = right_peak;
    float allowed = peak ? MUSIC_PCM_PEAK_LIMIT / (float)peak : max_gain;
    if (*ceiling > allowed) *ceiling = allowed;
    bool limited = gain > *ceiling;
    float applied = limited ? *ceiling : gain;
    out[0] = (int16_t)(left * applied);
    out[1] = (int16_t)(right * applied);
    *ceiling += release_step;
    if (*ceiling > max_gain) *ceiling = max_gain;
    return limited;
}

static inline int16_t clip16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

static int64_t pa_holdoff_until; // after a brownout event the amplifier stays off until this time

static void pa(bool on, bool *pa_on)
{
    if (on && esp_timer_get_time() < pa_holdoff_until) return; // still recovering from a brownout event
    if (on == *pa_on) return;
    if (on) es8388_dac_mute(false);
    gpio_set_level(BOARD_PA_ENABLE, on);
    if (!on) es8388_dac_mute(true);
    *pa_on = on;
    save_pa_flag(on);
}

// --------------------------------------------------------------- the task
static void audio_task(void *arg)
{
    int16_t block[AUDIO_IO_MAX_FRAMES * 2] = {0};
    int16_t mic[AUDIO_IO_MAX_FRAMES * 2] = {0};
    int16_t voice[AUDIO_IO_MAX_FRAMES] = {0}; // one 10 ms block of phone PCM (mono)
    unsigned last_epoch = UINT32_MAX;
    bool primed = false, pa_on = false, call_primed = false;
    float gain = 0.0f, ceiling = 1.0f;
    int16_t last_l = 0, last_r = 0;
    uint32_t phase = 0, phase2 = 0, in_peak = 0, out_peak = 0, mic_peak = 0, limited = 0;
    uint64_t frames_out = 0;
    int64_t prefill_since = esp_timer_get_time(), last_report = prefill_since, ring_since = prefill_since;
    int64_t last_guard_us = -(int64_t)POWER_GUARD_MIN_INTERVAL_MS * 1000LL, guard_rearm_at = 0;
    while (1) {
        unsigned epoch, mode, profile, rate, music_volume, call_volume, queued;
        bool music_active, ring, diagnose; source_t source;
        portENTER_CRITICAL(&lock);
        epoch = st.epoch; mode = st.test_mode; profile = st.dac_profile; source = st.source;
        music_active = st.music_active; ring = st.ring; music_volume = st.music_volume;
        call_volume = st.call_volume; queued = st.music_queued;
        rate = source == SRC_CALL ? st.call_rate : st.music_rate;
        diagnose = st.diagnose; st.diagnose = false;
        portEXIT_CRITICAL(&lock);
        if (diagnose) { es8388_log_readback(); audio_io_log_clocks(); }

        // ---------------- brownout guard ----------------
        if (power_guard_take_event()) {
            int64_t now_us = esp_timer_get_time();
            pa_on = false;                                   // the ISR already pulled the pin low
            es8388_dac_mute(true);
            save_pa_flag(false);
            pa_holdoff_until = now_us + POWER_GUARD_HOLDOFF_MS * 1000LL;
            unsigned lowered = profile;
            if (now_us - last_guard_us > POWER_GUARD_MIN_INTERVAL_MS * 1000LL && profile >= POWER_GUARD_DAC_STEP) {
                lowered = profile - POWER_GUARD_DAC_STEP;
                nvs_handle_t h;
                if (nvs_open("audio", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "dac", (uint8_t)lowered); nvs_commit(h); nvs_close(h); }
                boot_profile = lowered;
                portENTER_CRITICAL(&lock); st.dac_profile = lowered; ++st.epoch; portEXIT_CRITICAL(&lock);
            }
            last_guard_us = now_us;
            ESP_LOGW(TAG, "BROWNOUT GUARD: 3.3 V rail reached 2.80 V, amplifier cut in the interrupt (event %"PRIu32"). DAC profile %s P%u (%.1f dB). No reset, phone stays connected; audio resumes in %u ms",
                     power_guard_events(), lowered != profile ? "lowered to" : "stays at", lowered, (double)es8388_dac_profile_db(lowered), (unsigned)POWER_GUARD_HOLDOFF_MS);
            guard_rearm_at = now_us + POWER_GUARD_HOLDOFF_MS * 1000LL;
        }
        if (guard_rearm_at && esp_timer_get_time() >= guard_rearm_at) { guard_rearm_at = 0; power_guard_rearm(); }

        bool run_cal;
        portENTER_CRITICAL(&lock); run_cal = cal_pending && bt_core_ready(); if (run_cal) cal_pending = false; portEXIT_CRITICAL(&lock);
        if (run_cal) {
            pa(false, &pa_on);
            power_self_test();
            last_epoch = UINT32_MAX; // re-apply the (new) profile and reclock on the next pass
            continue;
        }

        if (epoch != last_epoch) {
            // Any source/rate/profile change: quiet the PA, reclock the bus, flush stale audio.
            pa(false, &pa_on);
            es8388_apply_dac_profile(profile);
            bool call = source == SRC_CALL;
            es8388_adc_power(call);
            ESP_ERROR_CHECK(audio_io_set_rate(rate, call));
            size_t n; void *stale;
            while ((stale = xRingbufferReceive(music_ring, &n, 0))) {
                portENTER_CRITICAL(&lock); st.music_queued -= ((packet_t *)stale)->length; portEXIT_CRITICAL(&lock);
                vRingbufferReturnItem(music_ring, stale);
            }
            while ((stale = xRingbufferReceiveUpTo(call_rx_ring, &n, 0, CALL_RX_BUFFER_BYTES))) vRingbufferReturnItem(call_rx_ring, stale);
            while ((stale = xRingbufferReceiveUpTo(call_tx_ring, &n, 0, CALL_TX_BUFFER_BYTES))) vRingbufferReturnItem(call_tx_ring, stale);
            gain = 0; last_l = last_r = 0; primed = call_primed = false; ceiling = 1.0f;
            prefill_since = ring_since = esp_timer_get_time(); last_epoch = epoch;
            ESP_LOGI(TAG, "Path: %s at %u Hz%s%s", call ? "CALL" : "MUSIC", rate,
                     ring ? " + ring tone" : "", mode ? " (test tone)" : "");
        }

        // ---------------- CALL: phone voice out, microphones in ----------------
        if (source == SRC_CALL) {
            size_t frames = rate / 100; // 10 ms blocks: 80 @ 8 kHz, 160 @ 16 kHz
            size_t want = frames * sizeof(int16_t), got = 0;
            if (!call_primed) {
                size_t waiting = CALL_RX_BUFFER_BYTES - xRingbufferGetCurFreeSize(call_rx_ring);
                if (waiting >= CALL_PREFILL_BYTES || esp_timer_get_time() - prefill_since > 300000) call_primed = true;
            }
            if (call_primed) got = ring_read_exact(call_rx_ring, CALL_RX_BUFFER_BYTES, voice, want);
            size_t have = got / sizeof(int16_t);
            if (call_primed && have < frames) { portENTER_CRITICAL(&lock); ++st.call_underruns; portEXIT_CRITICAL(&lock); }
            float level = call_volume / 15.0f;
            for (size_t i = 0; i < frames; ++i) {
                int32_t s = i < have ? voice[i] : 0;
                if (i >= have) s = last_l * (int32_t)(frames - i) / (int32_t)frames; // fade to silence
                int16_t v = clip16((int32_t)(s * level));
                block[2*i] = block[2*i+1] = v;
                last_l = i < have ? v : 0;
                uint32_t pk = v < 0 ? -(int32_t)v : v; if (pk > out_peak) out_peak = pk;
            }
            pa(true, &pa_on);
            if (!audio_io_write(block, frames)) { portENTER_CRITICAL(&lock); ++st.i2s_errors; portEXIT_CRITICAL(&lock); }
            // Microphones: same frame count keeps both directions in lock step.
            if (audio_io_read(mic, frames)) {
                int16_t mono[AUDIO_IO_MAX_FRAMES];
                for (size_t i = 0; i < frames; ++i) {
                    int32_t m = ((int32_t)mic[2*i] + mic[2*i+1]) / 2; // both onboard mics
                    mono[i] = clip16((int32_t)(m * MIC_DIGITAL_GAIN));
                    uint32_t pk = mono[i] < 0 ? -(int32_t)mono[i] : mono[i]; if (pk > mic_peak) mic_peak = pk;
                }
                if (xRingbufferSend(call_tx_ring, mono, frames * sizeof(int16_t), 0) != pdTRUE) {
                    portENTER_CRITICAL(&lock); ++st.call_tx_drops; portEXIT_CRITICAL(&lock);
                }
                esp_hf_client_outgoing_data_ready();
            } else {
                portENTER_CRITICAL(&lock); ++st.i2s_errors; portEXIT_CRITICAL(&lock);
            }
            frames_out += frames;
            goto report;
        }

        // ---------------- test tones / local ring tone ----------------
        bool ringing = ring && !music_active && !mode;
        if (mode || ringing) {
            size_t n; void *stale = xRingbufferReceive(music_ring, &n, 0);
            if (stale) {
                portENTER_CRITICAL(&lock); st.music_queued -= ((packet_t *)stale)->length; portEXIT_CRITICAL(&lock);
                vRingbufferReturnItem(music_ring, stale);
            }
            int64_t t = (esp_timer_get_time() - ring_since) / 1000 % (CALL_RING_ON_MS + CALL_RING_OFF_MS);
            bool burst = !ringing || t < CALL_RING_ON_MS;
            float level = music_volume / 100.0f, target = level * level;
            float step = 1.0f / (rate * 0.02f);
            uint32_t d1 = ((uint64_t)(mode == 2 ? 800 : mode == 1 ? 400 : 425) << 32) / rate;
            uint32_t d2 = ((uint64_t)480 << 32) / rate; // ring: 425 + 480 Hz dual tone
            for (int i = 0; i < 256; ++i) {
                float want = burst && mode != 3 ? target : 0.0f;
                if (gain < want) { gain += step; if (gain > want) gain = want; }
                else if (gain > want) { gain -= step; if (gain < want) gain = want; }
                int32_t s = sine32[phase >> 27];
                if (ringing) s = (s + sine32[phase2 >> 27]) / 2;
                phase += d1; phase2 += d2;
                int16_t v = (int16_t)(s * gain);
                block[2*i] = mode == 2 ? 0 : v;
                block[2*i+1] = mode == 1 ? 0 : v;
            }
            pa(true, &pa_on);
            if (!audio_io_write(block, 256)) { portENTER_CRITICAL(&lock); ++st.i2s_errors; portEXIT_CRITICAL(&lock); }
            goto report;
        }

        // ---------------- MUSIC ----------------
        bool consume = !music_active || primed || queued >= MUSIC_PREFILL_BYTES ||
                       esp_timer_get_time() - prefill_since > 150000;
        size_t item_size = 0;
        packet_t *p = consume ? xRingbufferReceive(music_ring, &item_size, 0) : NULL;
        if (p) {
            portENTER_CRITICAL(&lock); st.music_queued -= p->length; portEXIT_CRITICAL(&lock);
            if (!music_active || p->epoch != epoch) { vRingbufferReturnItem(music_ring, p); continue; }
            primed = true;
            pa(true, &pa_on);
            const int16_t *in = (const int16_t *)p->data;
            size_t frames = p->length / (p->channels * sizeof(int16_t));
            float level = music_volume / 100.0f, target = level * level; // digital gain 1x at 100%, peaks held at MUSIC_PCM_PEAK_LIMIT
            float step = 1.0f / (rate * 0.04f), release_step = 1.0f / (rate * 0.15f);
            for (size_t pos = 0; pos < frames;) {
                size_t n = frames - pos; if (n > 256) n = 256;
                for (size_t i = 0; i < n; ++i) {
                    if (gain < target) { gain += step; if (gain > target) gain = target; }
                    else if (gain > target) { gain -= step; if (gain < target) gain = target; }
                    size_t src = (pos + i) * p->channels;
                    int32_t il = in[src], ir = in[src + (p->channels == 2)];
                    uint32_t pk = il < 0 ? -il : il; if (pk > in_peak) in_peak = pk;
                    pk = ir < 0 ? -ir : ir; if (pk > in_peak) in_peak = pk;
                    if (scale_frame(il, ir, gain, 1.0f, &ceiling, release_step, &block[2*i])) ++limited;
                    last_l = block[2*i]; last_r = block[2*i+1];
                    pk = last_l < 0 ? -(int32_t)last_l : last_l; if (pk > out_peak) out_peak = pk;
                }
                if (!audio_io_write(block, n)) { portENTER_CRITICAL(&lock); ++st.i2s_errors; portEXIT_CRITICAL(&lock); break; }
                pos += n; frames_out += n;
            }
            vRingbufferReturnItem(music_ring, p);
        } else {
            if (primed && music_active) { portENTER_CRITICAL(&lock); ++st.music_underruns; portEXIT_CRITICAL(&lock); }
            for (int i = 0; i < 256; ++i) { // fade from the last sample, then rebuffer
                block[2*i] = (int32_t)last_l * (255-i) / 256;
                block[2*i+1] = (int32_t)last_r * (255-i) / 256;
            }
            audio_io_write(block, 256);
            last_l = last_r = 0; gain = 0;
            if (primed) { primed = false; prefill_since = esp_timer_get_time(); }
            if (!music_active) pa(false, &pa_on);
        }

report:
        if (esp_timer_get_time() - last_report >= 5000000) {
            portENTER_CRITICAL(&lock);
            uint32_t md = st.music_drops, mu = st.music_underruns, crd = st.call_rx_drops, ctd = st.call_tx_drops,
                     cu = st.call_underruns, ie = st.i2s_errors; unsigned q = st.music_queued;
            portEXIT_CRITICAL(&lock);
            ESP_LOGI(TAG, "%s %u Hz PA=%d in_peak=%"PRIu32" out_peak=%"PRIu32" mic_peak=%"PRIu32" frames=%"PRIu64" limited=%"PRIu32" P%u",
                     source == SRC_CALL ? "CALL" : "MUSIC", rate, pa_on, in_peak, out_peak, mic_peak, frames_out, limited, profile);
            ESP_LOGI(TAG, "music buffer=%u B drops=%"PRIu32" underruns=%"PRIu32" | call rx_drops=%"PRIu32" tx_drops=%"PRIu32" underruns=%"PRIu32" | i2s_errors=%"PRIu32,
                     q, md, mu, crd, ctd, cu, ie);
            if (power_guard_events()) ESP_LOGI(TAG, "brownout guard events since boot: %"PRIu32, power_guard_events());
            in_peak = out_peak = mic_peak = limited = 0; frames_out = 0;
            last_report = esp_timer_get_time();
        }
    }
}

// Decide the boot DAC profile from NVS:
//   calstep present  -> the previous run reset in the middle of the power
//                       self-test: that step collapsed the supply, the one
//                       below it is the limit (saved, test marked done).
//   paon == 1 + reset -> a reset while the amplifier was on in normal use:
//                       lower the saved profile by POWER_GUARD_DAC_STEP (fallback).
//   cal != 1         -> the self-test has not run yet: run it now.
static void load_boot_profile(void)
{
    esp_reset_reason_t why = esp_reset_reason();
    // Only a BROWNOUT counts as a supply collapse. The brownout detector is set
    // to 2.80 V (sdkconfig), above the IMP809 supervisor's 2.63 V, so it fires
    // first and leaves a distinct reset reason. A POWERON reset (power switch,
    // EN button, flash tool) is NOT a trip: before this rule, switching the
    // speaker off while playing lowered the saved level at the next boot.
    bool trip_reason = why == ESP_RST_BROWNOUT;
    unsigned profile = AUDIO_BOOT_DAC_PROFILE;
    bool cal_done = false;
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READWRITE, &h) != ESP_OK) { cal_pending = true; st.dac_profile = profile; return; }
    uint8_t v;
    if (nvs_get_u8(h, "dac", &v) == ESP_OK && v <= es8388_dac_profile_max()) profile = v;
    if (nvs_get_u8(h, "cal", &v) == ESP_OK && v == 1) cal_done = true;
    uint8_t step;
    if (nvs_get_u8(h, "calstep", &step) == ESP_OK) {
        nvs_erase_key(h, "calstep");
        if (trip_reason) {
            profile = step > CAL_MARGIN_STEPS ? step - 1 - CAL_MARGIN_STEPS : 0; // one below the step that tripped, minus the margin
            nvs_set_u8(h, "dac", (uint8_t)profile); nvs_set_u8(h, "cal", 1); cal_done = true;
            if (step > 0)
                ESP_LOGW(TAG, "POWER SELF-TEST result: the board reset at P%u (reset reason %d), so P%u (%.1f dB) is the loudest level this supply holds. Saved.",
                         step, (int)why, profile, (double)es8388_dac_profile_db(profile));
            else
                ESP_LOGE(TAG, "POWER SELF-TEST: the board reset even at P0 (-12 dB). This supply or cable cannot drive the speakers; staying at P0. Use a 5 V / 2 A adapter on the POWER port with a short, thick cable");
        } else {
            cal_done = false; // some other reset interrupted the test: run it again
        }
        nvs_commit(h);
    } else if (why == ESP_RST_POWERON && nvs_get_u8(h, "paon", &v) == ESP_OK && v == 1) {
        ESP_LOGW(TAG, "Reset while the amplifier was on (EN pin / power switch). If nobody switched it off, the supply collapsed faster than the brownout guard could cut the amplifier");
    } else if (trip_reason && nvs_get_u8(h, "paon", &v) == ESP_OK && v == 1) {
        if (profile >= POWER_GUARD_DAC_STEP) {
            profile -= POWER_GUARD_DAC_STEP;
            nvs_set_u8(h, "dac", (uint8_t)profile); nvs_commit(h);
            ESP_LOGW(TAG, "POWER TRIP: the board reset while the amplifier was on (reset reason %d). DAC profile lowered to P%u (%.1f dB) and saved; '0' re-runs the self-test",
                     (int)why, profile, (double)es8388_dac_profile_db(profile));
        } else {
            ESP_LOGW(TAG, "POWER TRIP at the lowest DAC profile: the supply or cable must be fixed");
        }
    }
    if (nvs_get_u8(h, "paon", &v) == ESP_OK && v) { nvs_set_u8(h, "paon", 0); nvs_commit(h); }
    pa_flag_nvs = false;
    nvs_close(h);
    cal_pending = !cal_done;
    boot_profile = profile;
    st.dac_profile = profile;
    ESP_LOGI(TAG, "DAC profile P%u (%.1f dB): %s", profile, (double)es8388_dac_profile_db(profile),
             cal_done ? "limit found by the power self-test for this supply" : "not yet calibrated, the power self-test runs now");
}

// Power self-test: a CAL_TONE_HZ tone at CAL_TONE_AMPLITUDE through both
// speakers, CAL_STEP_MS per DAC profile from P0 up to CAL_MAX_PROFILE. The step
// in progress is stored in NVS before it plays; if the supply collapses and the
// supervisor resets the chip, load_boot_profile() turns that into the limit.
static void power_self_test(void)
{
    unsigned max = CAL_MAX_PROFILE < es8388_dac_profile_max() ? CAL_MAX_PROFILE : es8388_dac_profile_max();
    ESP_LOGW(TAG, "POWER SELF-TEST: %u Hz tone at -3 dBFS on both speakers with the radio busy (inquiry), P0..P%u, %u ms each, to find the loudest level this supply holds. "
                  "A reset during the test is expected on a weak supply; the result is saved either way.", (unsigned)CAL_TONE_HZ, max, (unsigned)CAL_STEP_MS);
    bt_core_radio_load(true);
    vTaskDelay(pdMS_TO_TICKS(300)); // let the inquiry get going before the first step
    ESP_ERROR_CHECK(audio_io_set_rate(44100, false));
    es8388_adc_power(false);
    int16_t block[AUDIO_IO_MAX_FRAMES * 2];
    const uint32_t d = ((uint64_t)CAL_TONE_HZ << 32) / 44100;
    const unsigned ramp = 44100 / 20; // 50 ms fade in / out
    const unsigned per_step = 44100 * CAL_STEP_MS / 1000, total = per_step * (max + 1);
    uint32_t phase = 0; unsigned pos = 0; bool pa_on = false;
    for (unsigned p = 0; p <= max; ++p) {
        nvs_handle_t h;
        if (nvs_open("audio", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "calstep", (uint8_t)p); nvs_commit(h); nvs_close(h); }
        es8388_apply_dac_profile(p);
        pa(true, &pa_on);
        ESP_LOGI(TAG, "  self-test P%u (%.1f dB)", p, (double)es8388_dac_profile_db(p));
        unsigned done = 0; bool tripped = false;
        while (done < per_step && !tripped) {
            if (power_guard_take_event()) { tripped = true; break; }
            size_t n = per_step - done; if (n > AUDIO_IO_MAX_FRAMES) n = AUDIO_IO_MAX_FRAMES;
            for (size_t i = 0; i < n; ++i, ++pos) {
                int32_t s = sine32[phase >> 27] * (int32_t)(CAL_TONE_AMPLITUDE / 8192); // sine32 peaks at 8192
                if (pos < ramp) s = s * (int32_t)pos / (int32_t)ramp;
                else if (total - pos < ramp) s = s * (int32_t)(total - pos) / (int32_t)ramp;
                block[2*i] = block[2*i+1] = (int16_t)s;
                phase += d;
            }
            audio_io_write(block, n); done += n;
        }
        if (tripped) {
            // The guard cut the amplifier during step p: the supply holds p-1 at best.
            pa_on = false; // the ISR already pulled the pin low
            pa_holdoff_until = esp_timer_get_time() + POWER_GUARD_HOLDOFF_MS * 1000LL;
            unsigned limit = p > CAL_MARGIN_STEPS ? p - 1 - CAL_MARGIN_STEPS : 0;
            bt_core_radio_load(false);
            nvs_handle_t h2;
            if (nvs_open("audio", NVS_READWRITE, &h2) == ESP_OK) {
                nvs_erase_key(h2, "calstep"); nvs_set_u8(h2, "dac", (uint8_t)limit); nvs_set_u8(h2, "cal", 1); nvs_commit(h2); nvs_close(h2);
            }
            boot_profile = limit;
            portENTER_CRITICAL(&lock); st.dac_profile = limit; portEXIT_CRITICAL(&lock);
            es8388_dac_mute(true);
            ESP_LOGW(TAG, "POWER SELF-TEST: the brownout guard cut the amplifier at P%u (%.1f dB); saving P%u (%.1f dB) with %u step(s) of margin. No reset.",
                     p, (double)es8388_dac_profile_db(p), limit, (double)es8388_dac_profile_db(limit), (unsigned)CAL_MARGIN_STEPS);
            vTaskDelay(pdMS_TO_TICKS(POWER_GUARD_HOLDOFF_MS));
            power_guard_rearm();
            return;
        }
    }
    pa(false, &pa_on);
    bt_core_radio_load(false);
    unsigned result = max >= CAL_MARGIN_STEPS ? max - CAL_MARGIN_STEPS : 0;
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "calstep"); nvs_set_u8(h, "dac", (uint8_t)result); nvs_set_u8(h, "cal", 1); nvs_commit(h); nvs_close(h);
    }
    boot_profile = result;
    portENTER_CRITICAL(&lock); st.dac_profile = result; portEXIT_CRITICAL(&lock);
    ESP_LOGW(TAG, "POWER SELF-TEST passed up to P%u (%.1f dB); saving P%u (%.1f dB) with %u step(s) of margin for real streaming.",
             max, (double)es8388_dac_profile_db(max), result, (double)es8388_dac_profile_db(result), (unsigned)CAL_MARGIN_STEPS);
}

esp_err_t audio_engine_init(void)
{
    load_boot_profile();
    gpio_config_t pins = {.pin_bit_mask = 1ULL << BOARD_PA_ENABLE, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&pins));
    gpio_set_level(BOARD_PA_ENABLE, 0);
    ESP_ERROR_CHECK(es8388_init());
    music_ring = xRingbufferCreate(MUSIC_BUFFER_BYTES, RINGBUF_TYPE_NOSPLIT);
    call_rx_ring = xRingbufferCreate(CALL_RX_BUFFER_BYTES, RINGBUF_TYPE_BYTEBUF);
    call_tx_ring = xRingbufferCreate(CALL_TX_BUFFER_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!music_ring || !call_rx_ring || !call_tx_ring) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(audio_io_init(44100));
    ESP_ERROR_CHECK(power_guard_init());
    ESP_LOGI(TAG, "Engine: music 1x gain + limiter, DAC P%u, call volume %u/15, mic gain %.1fx",
             boot_profile, (unsigned)CALL_BOOT_VOLUME, (double)MIC_DIGITAL_GAIN);
    if (xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 6, NULL, 1) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
