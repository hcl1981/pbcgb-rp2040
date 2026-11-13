// Tontest fuer den alten PicoBoy Color: C-Dur-Tonleiter auf dem Lautsprecher
// (GP15), immer abwechselnd in zwei Varianten, endlos:
//
//   1. RECHTECK  - PWM mit der Notenfrequenz und 50 % Tastverhaeltnis, also
//                  hartes Ein/Aus wie tone(). Zwischen den Noten Pin auf 0.
//   2. EMULATOR  - wie die Tonausgabe des Game-Boy-Emulators: PWM-Traeger
//                  ~122 kHz (Wrap 1023), Pegel = Ruhepunkt 192 (19 %) +
//                  Sinus mit Ausschlag 448 (Stufe 6), unten bei 0 begrenzt,
//                  22 050 Werte pro Sekunde.
//
// Das Display zeigt Variante und Note. Systemtakt bleibt bei 125 MHz.

#include <math.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

#define ST7789_PICOBOY_IMPL
#include "st7789_picoboy.h"

#define SPK_PIN 15

static const float kNotes[] = { 261.63f, 293.66f, 329.63f, 349.23f,
                                392.00f, 440.00f, 493.88f, 523.25f };
static const char *const kNames[] = { "C4", "D4", "E4", "F4", "G4", "A4", "H4", "C5" };

static uint g_slice, g_chan;

static void show(const char *mode, const char *note) {
    const uint16_t bg = 0x0000;
    st7789_fill_rect(0, 100, PB_DISP_W, 60, bg);
    st7789_draw_text(16, 104, mode, st7789_color565(255, 220, 80), bg, 2);
    st7789_draw_text(16, 132, note, 0xFFFF, bg, 2);
}

static void speaker_off() {
    pwm_set_enabled(g_slice, false);
    gpio_init(SPK_PIN);
    gpio_set_dir(SPK_PIN, GPIO_OUT);
    gpio_put(SPK_PIN, 0);
}

// Variante 1: Rechteck mit Notenfrequenz
static void play_square(float freq, uint32_t ms) {
    const float div = 64.0f;                               // 125 MHz / 64
    uint32_t wrap = (uint32_t)(clock_get_hz(clk_sys) / div / freq) - 1;
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv(&c, div);
    pwm_config_set_wrap(&c, (uint16_t)wrap);
    gpio_set_function(SPK_PIN, GPIO_FUNC_PWM);
    pwm_init(g_slice, &c, false);
    pwm_set_chan_level(g_slice, g_chan, (uint16_t)(wrap / 2));
    pwm_set_enabled(g_slice, true);
    sleep_ms(ms);
    speaker_off();
}

// Variante 2: wie der Emulator - schneller Traeger, Sinus um Ruhepegel
static void play_emulator_style(float freq, uint32_t ms) {
    const int mid = 192, swing = 448, wrap = 1023;
    const uint32_t rate = 22050;
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv(&c, 1.0f);
    pwm_config_set_wrap(&c, wrap);
    gpio_set_function(SPK_PIN, GPIO_FUNC_PWM);
    pwm_init(g_slice, &c, true);

    float phase = 0.0f, step = 2.0f * (float)M_PI * freq / (float)rate;
    uint32_t n = rate * ms / 1000;
    absolute_time_t t = get_absolute_time();
    for (uint32_t i = 0; i < n; ++i) {
        int v = mid + (int)(swing * sinf(phase));
        if (v < 0) v = 0;
        if (v > wrap) v = wrap;
        pwm_set_chan_level(g_slice, g_chan, (uint16_t)v);
        phase += step;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        t = delayed_by_us(t, 1000000 / rate);
        busy_wait_until(t);
    }
    // sanft zurueck auf 0, dann Pin aus
    for (int v = mid; v > 0; v -= 4) { pwm_set_chan_level(g_slice, g_chan, (uint16_t)v); sleep_us(500); }
    speaker_off();
}

int main() {
    g_slice = pwm_gpio_to_slice_num(SPK_PIN);
    g_chan  = pwm_gpio_to_channel(SPK_PIN);
    speaker_off();

    st7789_init();
    st7789_fill(0x0000);
    st7789_draw_text(16, 40, "AUDIO TEST", 0xFFFF, 0x0000, 3);
    st7789_draw_text(16, 76, "Lautsprecher GP15", st7789_color565(160, 160, 160), 0x0000, 1);

    while (true) {
        for (int i = 0; i < 8; ++i) {
            show("1 RECHTECK", kNames[i]);
            play_square(kNotes[i], 400);
            sleep_ms(100);
        }
        sleep_ms(800);
        for (int i = 0; i < 8; ++i) {
            show("2 EMULATOR", kNames[i]);
            play_emulator_style(kNotes[i], 400);
            sleep_ms(100);
        }
        sleep_ms(800);
    }
}
