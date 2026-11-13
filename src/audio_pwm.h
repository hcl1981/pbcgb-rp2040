/*
 * audio_pwm.h
 *
 * PWM-Audio-Layer fuer den Piezo am PicoBoy Color (GP15, mit RC-Tiefpass davor).
 *
 * Strategie:
 *  - PWM-Slice 7B auf GP15, Wrap=1023, Trager bei ~195 kHz @ 200 MHz Sysclk.
 *  - Repeating Timer alle 22.7 us (44100 Hz) konsumiert Samples aus einem
 *    Ringpuffer und schreibt sie in das PWM Compare-Register.
 *  - Stereo-16-bit-Samples vom minigb_apu werden zu 10-bit-Mono gemischt.
 *  - Ringpuffer ist 4096 Samples gross (~93 ms Audio-Pufferung).
 *
 * Single-Header: in genau EINER .cpp vor dem Include
 *   #define AUDIO_PWM_IMPL
 * setzen.
 */

#ifndef AUDIO_PWM_H
#define AUDIO_PWM_H

#include <pico/stdlib.h>
#include <hardware/pwm.h>
#include <hardware/gpio.h>
#include <pico/time.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AUDIO_PWM_PIN
#define AUDIO_PWM_PIN  15
#endif

#ifndef AUDIO_PWM_WRAP
#define AUDIO_PWM_WRAP 1023u    // 10-bit
#endif

#ifndef AUDIO_PWM_BUF
#define AUDIO_PWM_BUF  4096u    // muss Zweierpotenz sein
#endif

// Ruhepunkt (Stille) und Ausschlag des PWM-Pegels. Vorgabe: Mitte, voller
// Ausschlag. Haengt der Lautsprecher an einem Transistor, fliesst beim
// Ruhepunkt dauerhaft Strom durch die Spule - der alte PicoBoy Color startete
// bei 50 % neu. Kleinerer Ruhepunkt = weniger Strom, dafuer leiser.
#ifndef AUDIO_PWM_MID
#define AUDIO_PWM_MID   (AUDIO_PWM_WRAP / 2)
#endif
#ifndef AUDIO_PWM_SWING
#define AUDIO_PWM_SWING (AUDIO_PWM_WRAP / 2)
#endif
// Pegel beim Start langsam von 0 auf den Ruhepunkt fahren (ms, 0 = sofort).
#ifndef AUDIO_PWM_RAMP_MS
#define AUDIO_PWM_RAMP_MS 0
#endif

void audio_pwm_init(uint32_t sample_rate);
void audio_pwm_push(const int16_t *stereo, size_t samples);
void audio_pwm_set_volume(uint8_t v);   // 0..8 (0=mute, 4=normal)
void audio_pwm_mute(bool m);
// Ruhepunkt und Ausschlag (PWM-Stufen 0..AUDIO_PWM_WRAP), vor audio_pwm_init.
void audio_pwm_set_bias(uint16_t mid, uint16_t swing);
// 1-Bit-Ausgabe: Pegel "high" (0..AUDIO_PWM_WRAP), wenn der Ton ueber null
// liegt, sonst 0 - wie ein Rechteckton. Stille = Pin aus. Fuer Lautsprecher-
// stufen, die auf Analogpegel (schnelles PWM) nicht reagieren.
void audio_pwm_set_onebit(uint16_t high);
// Puls-Ausgabe: bei jedem Anstieg der Tonkurve ueber null ein kurzer, voller
// Puls von "samples" Abtastwerten (bei 22 050 Hz je 45 us), hoechstens ein
// Viertel der zuletzt gemessenen Periode; sonst Pin aus. Wenig Strom, die
// Tonhoehe bleibt erhalten.
void audio_pwm_set_pulse(uint8_t samples);

#ifdef __cplusplus
}
#endif

// =====================================================================
//  Implementierung
// =====================================================================
#ifdef AUDIO_PWM_IMPL

static volatile uint16_t g_audio_buf[AUDIO_PWM_BUF];
static volatile uint32_t g_audio_head = 0;     // schreiben (Producer)
static volatile uint32_t g_audio_tail = 0;     // lesen (Consumer/IRQ)
static uint8_t  g_audio_volume = 16;           // 0..32, 8 = unity, 16 = 2x, 32 = 4x
static volatile bool g_audio_mute = false;
static uint     g_audio_slice = 0;
static uint     g_audio_chan  = 0;
static repeating_timer_t g_audio_timer;
static uint16_t g_audio_mid   = AUDIO_PWM_MID;
static uint16_t g_audio_swing = AUDIO_PWM_SWING;
static bool     g_audio_onebit = false;
static uint16_t g_audio_high   = 0;
static uint8_t  g_audio_pulse  = 0;    // >0: Puls-Ausgabe, Pulslaenge in Samples

void audio_pwm_set_onebit(uint16_t high) {
    if (high > AUDIO_PWM_WRAP) high = AUDIO_PWM_WRAP;
    g_audio_onebit = true;
    g_audio_high   = high;
    g_audio_mid    = 0;      // Ruhe = Pin aus
    g_audio_swing  = 0;
}

void audio_pwm_set_pulse(uint8_t samples) {
    audio_pwm_set_onebit(AUDIO_PWM_WRAP);   // Puls = voll an
    g_audio_pulse = samples ? samples : 1;
}

void audio_pwm_set_bias(uint16_t mid, uint16_t swing) {
    if (mid > AUDIO_PWM_WRAP) mid = AUDIO_PWM_WRAP;
    g_audio_mid = mid;
    g_audio_swing = swing;
}

static bool __not_in_flash_func(audio_pwm_timer_cb)(repeating_timer_t *t) {
    (void)t;
    uint32_t tail = g_audio_tail;
    uint16_t s;
    if (tail == g_audio_head) {
        s = g_audio_mid;        // Puffer leer: Ruhepunkt (Stille)
    } else {
        s = g_audio_buf[tail & (AUDIO_PWM_BUF - 1)];
        g_audio_tail = tail + 1;
    }
    if (g_audio_mute) s = g_audio_mid;
    pwm_set_chan_level(g_audio_slice, g_audio_chan, s);
    return true;
}

void audio_pwm_init(uint32_t sample_rate) {
    gpio_set_function(AUDIO_PWM_PIN, GPIO_FUNC_PWM);
    g_audio_slice = pwm_gpio_to_slice_num(AUDIO_PWM_PIN);
    g_audio_chan  = pwm_gpio_to_channel(AUDIO_PWM_PIN);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, AUDIO_PWM_WRAP);
    pwm_config_set_clkdiv(&cfg, 1.0f);     // voller Sysclock-Trager
    pwm_set_chan_level(g_audio_slice, g_audio_chan, 0);
    pwm_init(g_audio_slice, &cfg, true);
    // Sanft auf den Ruhepunkt fahren statt schlagartig (Einschaltstrom)
#if AUDIO_PWM_RAMP_MS > 0
    for (uint32_t t = 1; t <= AUDIO_PWM_RAMP_MS; ++t) {
        pwm_set_chan_level(g_audio_slice, g_audio_chan,
                           (uint16_t)((uint32_t)g_audio_mid * t / AUDIO_PWM_RAMP_MS));
        sleep_ms(1);
    }
#endif
    pwm_set_chan_level(g_audio_slice, g_audio_chan, g_audio_mid);

    // Ruhepunkt in den Ringpuffer als sichere Anfangsfuellung
    for (uint32_t i = 0; i < AUDIO_PWM_BUF; ++i) g_audio_buf[i] = g_audio_mid;
    g_audio_head = AUDIO_PWM_BUF / 2;       // halbvoll starten
    g_audio_tail = 0;

    // Sample-Timer: -1us heisst "1us nach dem letzten Aufruf" (constant rate)
    int64_t period_us = -(int64_t)(1000000 / sample_rate);
    add_repeating_timer_us(period_us, audio_pwm_timer_cb, NULL, &g_audio_timer);
}

void audio_pwm_set_volume(uint8_t v) {
    if (v > 32) v = 32;
    g_audio_volume = v;
}

void audio_pwm_mute(bool m) { g_audio_mute = m; }

void __not_in_flash_func(audio_pwm_push)(const int16_t *stereo, size_t samples) {
    uint32_t head = g_audio_head;
    uint32_t tail = g_audio_tail;
    uint32_t mask = AUDIO_PWM_BUF - 1;
    int vol = g_audio_volume;
    for (size_t i = 0; i < samples; ++i) {
        // Stereo zu Mono mischen
        int32_t s = ((int32_t)stereo[2*i] + (int32_t)stereo[2*i+1]) >> 1;
        // Lautstaerke 0..8 mappen (>>3 = unity)
        s = (s * vol) >> 3;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        // 16-bit signed -> PWM-Pegel um den Ruhepunkt (oder 1 Bit)
        int32_t v;
        if (g_audio_pulse) {
            // Puls bei jedem Anstieg ueber null, Laenge begrenzt auf 1/4 Periode
            static bool     above = false;
            static uint32_t since_rise = 0, last_period = 64, pulse_left = 0;
            ++since_rise;
            bool now_above = s > 256;
            if (now_above && !above) {
                last_period = since_rise;
                since_rise = 0;
                uint32_t cap = last_period / 4;
                if (cap < 1) cap = 1;
                pulse_left = g_audio_pulse < cap ? g_audio_pulse : cap;
            }
            above = now_above;
            if (pulse_left) { v = g_audio_high; --pulse_left; }
            else            { v = 0; }
        } else if (g_audio_onebit) {
            v = s > 256 ? (int32_t)g_audio_high : 0;
        } else {
            v = (int32_t)g_audio_mid + ((s * (int32_t)g_audio_swing) >> 15);
        }
        if (v < 0) v = 0;
        if (v > (int32_t)AUDIO_PWM_WRAP) v = AUDIO_PWM_WRAP;
        uint16_t u = (uint16_t)v;

        // Wenn voll, ueberschreiben wir den aeltesten Sample
        if (((head + 1) & mask) == (tail & mask)) {
            tail = (tail + 1) & 0xFFFFFFFFu;
            g_audio_tail = tail;
        }
        g_audio_buf[head & mask] = u;
        head = (head + 1) & 0xFFFFFFFFu;
    }
    g_audio_head = head;
}

#endif // AUDIO_PWM_IMPL
#endif // AUDIO_PWM_H
