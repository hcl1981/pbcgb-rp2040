/*
 * PicoBoyGB.ino
 *
 * Game-Boy-DMG-Emulator fuer RP2040 (2 MB Flash), zwei Geraete (picoboy_pins.h):
 *   - 2-Zoll-Waveshare-Display 19888 (ST7789, 240x320) mit Einzeltasten
 *   - alter PicoBoy Color (ST7789 240x280, Joystick, A/B, Piezo)
 * Basis: Peanut-GB von Mahyar Koshkouei (MIT).
 * Port: ILI9225/SD/I2S -> ST7789/Flash/PWM-Piezo, RP2040 -> RP2350 (PicoBoy
 * Color Plus) und von dort zurueck auf den RP2040 mit 2 MB Flash.
 *
 * Steuerung PicoBoy Color: Joystick = D-Pad, A/B = A/B,
 *   Mitte + B = START, Mitte + A = SELECT, USB-Stick-Modus: Mitte + RESET.
 *
 * Steuerung Waveshare-Geraet:
 *   UP/DOWN/LEFT/RIGHT (GP8/GP7/GP9/GP6)  ->  GB D-Pad
 *   A (GP4)                               ->  GB A
 *   B (GP5)                               ->  GB B
 *   LEFT+RIGHT gleichzeitig (oder GP1)    ->  GB START
 *   UP+DOWN gleichzeitig (oder GP0)       ->  GB SELECT
 *
 * Modus-Verzweigung beim Boot:
 *   B halten + RESET/Einschalten  ->  USB-Stick-Modus.
 *     Display zeigt einen Hinweis im GB-Bildschirm-Bereich. PC sieht
 *     das Geraet als USB-Drive PICOBOYGB. .gb-Files draufziehen, ejecten.
 *     Modus verlassen: RESET/Einschalten OHNE B zu halten.
 *   RESET/Einschalten OHNE B  ->  Spielmodus.
 *     Es erscheint IMMER das Hauptmenue (Info, .gb-Files), egal
 *     wie viele ROMs in FatFS liegen. Bei 0 Files muss der User per
 *     B+RESET in den USB-Stick-Modus wechseln zum Hochladen.
 *
 *  Nach Auswahl im Menue liest der Emulator das ROM direkt aus seiner Datei
 *  im Flash (XIP, siehe flash_fs.h). Liegt die Datei nicht am Stueck, wird
 *  sie vorher einmal zusammenhaengend umkopiert. Dann wird der
 *  Steuerungs-Splash 2 Sekunden gezeigt und der Emulator gestartet.
 */

// ---- Konfiguration ----
#define ENABLE_LCD               1
#include "picoboy_pins.h"   // Geraeteauswahl, frueh: ENABLE_SOUND haengt davon ab
#define ENABLE_SOUND             PB_HAS_SPEAKER
#define PEANUT_GB_HIGH_LCD_ACCURACY 0   // 0 = schneller, kleine optische Abstriche
#define PEANUT_GB_USE_BIOS       0

// 0 = 160x144 zentriert, schwarzer Rand    (am schnellsten)
// 1 = 1.25x auf 200x180 zentriert          (besserer Display-Fuellgrad)
#define DISPLAY_SCALE_MODE       1

// SPI-Takt fuer ST7789. Bei sysclk 250 MHz sind nur 125 oder 62.5 MHz
// erreichbar (RP2350-Divider rastet auf diese Werte). Bei 1.25x-Skalierung
// ist 125 MHz noetig damit das Display-DMA in das Frame-Budget passt.
// Wenn das Modul Streifen/Glitches zeigt: auf 62500000u zurueckstellen.
// Muss VOR dem Include von st7789_picoboy.h gesetzt sein.
#ifndef ST7789_SPI_HZ
#define ST7789_SPI_HZ            125000000u
#endif

// RP2040-Takt, in CMakeLists.txt einstellbar (CPU_KHZ).
#ifndef SYS_CLOCK_KHZ
#define SYS_CLOCK_KHZ            250000
#endif

// Core-Spannung. Default ist VREG_VOLTAGE_1_10 (1.10V). Bei Batteriebetrieb
// sinkt die Versorgungsspannung mit dem Entladezustand und VDDCORE kann
// kurzzeitig einbrechen -> bei 250 MHz instabil. Mit 1.25V hat der interne
// Regler ~0.15V Puffer bevor wir in den kritischen Bereich kommen.
//   ENABLE_VREG_BOOST 0 = Default 1.10V (USB-Strom)
//   ENABLE_VREG_BOOST 1 = wende VREG_BOOST_LEVEL an (Batterie)
// Werte: VREG_VOLTAGE_1_15 / VREG_VOLTAGE_1_20 / VREG_VOLTAGE_1_25 / VREG_VOLTAGE_1_30
// RP2040: 250 MHz laufen ueblicherweise mit 1.20V; in CMakeLists.txt (CPU_VREG).
#define ENABLE_VREG_BOOST        1
#ifndef VREG_BOOST_LEVEL
#define VREG_BOOST_LEVEL         VREG_VOLTAGE_1_20
#endif

// Backlight-Helligkeit ueber PWM (0..255). 255 = volle Helligkeit, 100 = ca. 40 %.
// Niedrigere Werte sparen Strom. Da das gefuehlt dunkler wirkt, wird die
// Palette ueber BACKLIGHT_PALETTE_LIFT entsprechend aufgehellt - so wirkt
// das Bild trotz schwaecherem Backlight gut sichtbar.
//
// Faustregel: BACKLIGHT_LEVEL  60 -> LIFT 80
//             BACKLIGHT_LEVEL 100 -> LIFT 60
//             BACKLIGHT_LEVEL 150 -> LIFT 30
//             BACKLIGHT_LEVEL 255 -> LIFT 0
#define BACKLIGHT_LEVEL          100
#define BACKLIGHT_PALETTE_LIFT    60

// Feste Farbpalette beim Start (Index aus gbcolors.h, 0..12).
// Aequivalent zu "im alten Hotkey-Modus 4x RIGHT ab Default 0".
#define DEFAULT_PALETTE_INDEX    4

// Audio kann zur Laufzeit ueber das Menue an/aus geschaltet werden.
// Default OFF: bei Batteriebetrieb kann lautes PWM-Audio die Versorgungs-
// spannung kurz druecken und Stabilitaetsprobleme bei 250 MHz ausloesen.
// Im Menue per LEFT/RIGHT auf "Audio: OFF" einschaltbar.
static bool g_audio_enabled = false;
// Ton aus/an. "An" ist die erste Einstellung, die dem Nutzer am besten
// gefiel ("Stufe 6"): analoger PWM (195 kHz), Ruhepegel 320/1023 (31 %),
// Ausschlag 320. Erfahrungen am alten PicoBoy Color (USB-Log): Ruhepegel
// 50 % -> sofort Stromausfall, 31 % mit Ausschlag 512 -> Stromausfall nach
// gut einer Minute. 1-Bit- und Puls-Ausgabe klangen nicht besser.
static int g_audio_level = 0;
#define AUDIO_LEVELS 1
static const char *const kAudioNames[AUDIO_LEVELS + 1] = { "OFF", "ON" };
static const unsigned kAudioMid   = 320;
static const unsigned kAudioSwing = 320;

// Feste Menue-Eintraege vor der Spieleliste: Info und (nur mit Ton) Audio.
#define MENU_SPECIAL_COUNT  (ENABLE_SOUND ? 2 : 1)
#define MENU_AUDIO_ITEM     (ENABLE_SOUND ? 1 : -1)

// ---- Pico SDK (no Arduino core) ----
#include <string.h>
#include <stdio.h>
#include <strings.h>   // strcasecmp
#include <hardware/clocks.h>
#include <hardware/flash.h>
#include <hardware/sync.h>
#include <hardware/gpio.h>
#include <hardware/watchdog.h>
#include <hardware/vreg.h>
#include <hardware/pwm.h>
#include <hardware/dma.h>
#include <pico/multicore.h>
#include <pico/time.h>
#include <pico/stdlib.h>

// ---- Arduino compatibility shims -----------------------------------------
// The original sketch used a handful of Arduino runtime helpers. Map them to
// their pico-sdk equivalents so the emulator/menu code stays untouched.
static inline uint32_t millis() { return to_ms_since_boot(get_absolute_time()); }
static inline void delay(uint32_t ms) { sleep_ms(ms); }

// ===========================================================================
// Build-Time Guards
// ---------------------------------------------------------------------------
// Clear compile errors instead of a mysterious runtime hang if the CMake
// board/flash configuration is wrong. See CMakeLists.txt (PICO_BOARD=pico,
// PICO_PLATFORM=rp2040, PICO_FLASH_SIZE_BYTES=2 MB).
// ===========================================================================
#if !defined(PICO_RP2040) || !PICO_RP2040
  #error "Wrong platform: this firmware is for the RP2040 (original PicoBoy Color). " \
         "Build with -DPICO_PLATFORM=rp2040 -DPICO_BOARD=pico."
#endif

// The FatFs partition and the save region reach up to 2 MB (flash_fs.h).
#if !defined(PICO_FLASH_SIZE_BYTES) || PICO_FLASH_SIZE_BYTES < (2 * 1024 * 1024)
  #error "Wrong flash size: build with -DPICO_FLASH_SIZE_BYTES=2097152 (2 MB)."
#endif

// FatFs partition (flash-backed) + USB mass-storage export replace the
// arduino-pico FatFS/FatFSUSB libraries used by the original sketch.
extern "C" {
  #include "ff.h"        // ChaN FatFs (vendored under ../fatfs)
  #include "tusb.h"      // TinyUSB device stack (MSC)
}
#include "flash_fs.h"    // flash layout + block device

#include "pb_input.h"

#define ST7789_PICOBOY_IMPL
#include "st7789_picoboy.h"

// Boot-Splash-Bilddaten (240x280 RGB565, const -> liegt im Flash).

#define AUDIO_PWM_IMPL
// 2048 Samples bei 22 050 Hz = 93 ms, so viel wie 4096 bei 44,1 kHz auf dem PBC+.
#define AUDIO_PWM_BUF  2048u
#include "audio_pwm.h"

// minigb_apu.h MUSS vor peanut_gb.h stehen: peanut_gb.h ruft mit ENABLE_SOUND=1
// direkt audio_read()/audio_write() auf, die Deklarationen liegen in minigb_apu.h.
#include "minigb_apu.h"

extern "C" {
  #include "peanut_gb.h"
  #include "gbcolors.h"
}

#include "rom.h"

// ===========================================================================
// FatFs helpers (flash-backed volume, drive "")
// ---------------------------------------------------------------------------
// Thin wrappers so the menu/ROM code reads like the original FatFS.* calls.
// The volume lives in the flash region defined in flash_fs.h and is exported
// over USB in the stick mode below.
// ===========================================================================
static FATFS g_fatfs;

static bool fs_mount() {
    return f_mount(&g_fatfs, "", 1) == FR_OK;   // 1 = mount now
}

static void fs_unmount() {
    f_mount(nullptr, "", 0);
}

// Arbeitspuffer fuer f_mkfs: der zweite Frame-Buffer (unten), der vor dem
// Emulatorstart frei ist. Spart auf dem RP2040 4 KB RAM.
static uint8_t *fs_scratch_4k();

static bool fs_format() {
    BYTE *work = fs_scratch_4k();
    const UINT work_size = FF_MAX_SS;
    // FM_SFD: no partition table (single FAT volume filling the region),
    // matching how the host sees a small USB stick.
    MKFS_PARM opt = { FM_FAT | FM_SFD, 0, 0, 0, 0 };
    if (f_mkfs("", &opt, work, work_size) != FR_OK) return false;
    if (fs_mount()) {
        f_setlabel("PICOBOYGB");
        fs_unmount();
    }
    return true;
}

// ===========================================================================
// Globaler Emulator-State
// ===========================================================================
static struct gb_s gb;
static uint8_t cart_ram[32768];          // max. GB-Cart-RAM-Groesse
// Bank 0 (0000-3FFF) im SRAM. Auf dem PBC+ waren es 64 KB; der RP2040 hat
// nur 264 KB RAM, und die umgeschalteten Baenke kommen ohnehin aus dem Flash.
static uint8_t rom_bank0_cache[16384];
static palette_t palette;
static uint8_t  manual_palette = 0;
static const uint8_t *active_rom = nullptr;
static uint32_t active_rom_len = 0;

static int16_t audio_buffer[AUDIO_SAMPLES * 2];   // Stereo

// ===========================================================================
// Display-Geometrie
// ===========================================================================
#if DISPLAY_SCALE_MODE == 0
  static const uint16_t kViewW = 160;
  static const uint16_t kViewH = 144;
  static const uint16_t kViewX = (PB_DISP_W - kViewW) / 2;   // 40
  static const uint16_t kViewY = (PB_DISP_H - kViewH) / 2;   // 88
#else
  // 1.25x: 200x180, zentriert. 4 Eingangspixel werden zu 5 Ausgangspixeln,
  // sowohl horizontal als auch vertikal. Doppelt so wenig Mehrarbeit wie
  // 1.5x bei gleichzeitig besserem Display-Fuellgrad als 1:1.
  static const uint16_t kViewW = 200;
  static const uint16_t kViewH = 180;
  static const uint16_t kViewX = (PB_DISP_W - kViewW) / 2;   // 20
  static const uint16_t kViewY = (PB_DISP_H - kViewH) / 2;   // 70
#endif

// Eine Output-Scanline (16-bit Pixel)
static uint16_t scanline_buf[kViewW];

// ===========================================================================
// Peanut-GB Callbacks
// ===========================================================================

extern "C" uint8_t __not_in_flash_func(gb_rom_read)(struct gb_s *, const uint_fast32_t addr) {
    if (addr < sizeof(rom_bank0_cache)) return rom_bank0_cache[addr];
    if (addr < active_rom_len) return active_rom[addr];
    return 0xFF;
}

extern "C" uint8_t __not_in_flash_func(gb_cart_ram_read)(struct gb_s *, const uint_fast32_t addr) {
    return cart_ram[addr];
}

extern "C" void __not_in_flash_func(gb_cart_ram_write)(struct gb_s *, const uint_fast32_t addr, const uint8_t v) {
    cart_ram[addr] = v;
}

extern "C" void gb_error(struct gb_s *, const enum gb_error_e, const uint16_t) {
    // Fehler ignorieren - viele Spiele haben harmlose "invalid" Reads.
}

// ---------------------------------------------------------------------------
// Pre-baked Palette-LUT (256 Eintraege, ~512 Byte SRAM, im RAM gehalten).
// Statt pro Pixel `palette[(p>>4)&3][p&3]` - zwei Indirektionen - direkt
// lookup_palette[p]. Wird nur bei Palettenwechsel neu gefuellt.
// ---------------------------------------------------------------------------
static uint16_t fast_palette[256];

static void rebuild_fast_palette() {
    for (int i = 0; i < 256; ++i) {
        fast_palette[i] = palette[(i & LCD_PALETTE_ALL) >> 4][i & 3];
    }
}

// ---------------------------------------------------------------------------
// Frame-Buffer + Core 1 Display-Worker
// ---------------------------------------------------------------------------
// Zwei Frame-Buffer (RGB565). Core 0 emuliert in fb_write, Core 1 schiebt
// fb_send per DMA zum SPI raus. Frame-Ende: Pointer tauschen, Mailbox an
// Core 1. So sind Emulation und Display-Output entkoppelt - CPU0 wartet
// nicht mehr auf SPI.
//   1:1   (SCALE_MODE 0): 160 x 144 = 46 KB pro Buffer, 92 KB total
//   1.25x (SCALE_MODE 1): 200 x 180 = 72 KB pro Buffer, 144 KB total
// ---------------------------------------------------------------------------
#define FB_W   kViewW
#define FB_H   kViewH

static uint16_t fb_a[kViewW * kViewH];
static uint16_t fb_b[kViewW * kViewH];
static uint16_t *fb_write = fb_a;   // Core 0 schreibt hier rein
static uint16_t *fb_send  = fb_b;   // Core 1 liest hier raus

static uint8_t *fs_scratch_4k() {
    static_assert(sizeof(fb_b) >= FF_MAX_SS, "frame buffer too small as scratch");
    return (uint8_t *)fb_b;
}

static int      g_dma_chan = -1;    // DMA-Channel-Nummer (in setup1() reserviert)

// ---------------------------------------------------------------------------
// Dirty-Box-Tracking: nur der Bereich des Frame-Buffers, der sich gegenueber
// dem vorherigen Frame geaendert hat, wird per SPI gesendet. Bei statischen
// Spielen (RPGs, Tetris) spart das massiv SPI-Bandbreite. Bei voll bewegtem
// Bild ist die Box gleich dem ganzen Frame -> kein Verlust.
//
// Werte sind Output-Buffer-Koordinaten (0..kViewW-1, 0..kViewH-1).
// "Leer" wenn max_x < 0 oder max_y < 0 (nichts dirty).
// ---------------------------------------------------------------------------
static int g_dirty_min_x;
static int g_dirty_max_x;
static int g_dirty_min_y;
static int g_dirty_max_y;

static inline void __not_in_flash_func(dirty_reset)() {
    g_dirty_min_x =  kViewW;
    g_dirty_max_x = -1;
    g_dirty_min_y =  kViewH;
    g_dirty_max_y = -1;
}

static inline void __not_in_flash_func(dirty_mark_row)(int y, int min_x, int max_x) {
    if (max_x < 0) return;   // diese Zeile hatte keine Aenderungen
    if (min_x < g_dirty_min_x) g_dirty_min_x = min_x;
    if (max_x > g_dirty_max_x) g_dirty_max_x = max_x;
    if (y     < g_dirty_min_y) g_dirty_min_y = y;
    if (y     > g_dirty_max_y) g_dirty_max_y = y;
}

// Verpackt die Dirty-Box in ein FIFO-Wort (4 vorzeichenlose Bytes).
// Frueher wurden die Bytes als int8_t gelesen: bei 1.25x (200x180) sind
// x/y ab 128 dann negativ, Kern 1 hielt fast jedes Bild fuer "unveraendert"
// und schickte nur Teile davon -> schwarzes Bild, nur einzelne Stuecke.
#define DIRTY_BOX_EMPTY 0xFFFFFFFFu   // unmoeglich als echte Box (min_x <= 199)

static inline uint32_t __not_in_flash_func(dirty_pack)() {
    if (g_dirty_max_x < 0 || g_dirty_max_y < 0) return DIRTY_BOX_EMPTY;
    static_assert(kViewW <= 256 && kViewH <= 256, "box coordinates must fit in a byte");
    return ((uint32_t)g_dirty_min_x << 24)
         | ((uint32_t)g_dirty_max_x << 16)
         | ((uint32_t)g_dirty_min_y <<  8)
         |  (uint32_t)g_dirty_max_y;
}

// Core 1 Worker-Loop: blockiert auf der Inter-Core-FIFO. Wenn Core 0 einen
// Frame-Pointer schickt, wird er per DMA zum SPI geschoben. Nach DMA-Fertig
// wird ein "ack"-Wert zurueckgeschickt damit Core 0 weiss, dass der Buffer
// jetzt frei zum Wiederverwenden ist.
static void __not_in_flash_func(core1_display_worker)() {
    // Initialen Ack rausschicken: signalisiert Core 0 "bin bereit, Buffer
    // frei" - sonst wuerde der erste pop_blocking() in lcd_draw_line bei
    // line==143 fuer immer haengen, weil Core 1 noch nichts gesendet hat.
    multicore_fifo_push_blocking(1);

    while (true) {
        // Pointer auf den fertigen Frame-Buffer holen (blockiert).
        uint32_t raw_frame = multicore_fifo_pop_blocking();
        // Bounding-Box als zweite Nachricht: 4x 8-bit, sichtbar als signed
        // nach Recovery (max-Felder koennen -1 sein wenn nichts dirty).
        uint32_t raw_box   = multicore_fifo_pop_blocking();
        const uint16_t *frame = (const uint16_t *)raw_frame;

        // Werte 0..199 bzw. 0..179 als vorzeichenlose Bytes, "nichts
        // geaendert" als eigener Wert (siehe dirty_pack()).
        int box_min_x, box_max_x, box_min_y, box_max_y;
        if (raw_box == DIRTY_BOX_EMPTY) {
            box_min_x = box_min_y = 0;
            box_max_x = box_max_y = -1;
        } else {
            box_min_x = (int)((raw_box >> 24) & 0xFF);
            box_max_x = (int)((raw_box >> 16) & 0xFF);
            box_min_y = (int)((raw_box >>  8) & 0xFF);
            box_max_y = (int)( raw_box        & 0xFF);
        }

        if (box_max_x < 0 || box_max_y < 0) {
            // Nichts dirty - sofort ack zurueck.
            multicore_fifo_push_blocking(1);
            continue;
        }

        int box_w = box_max_x - box_min_x + 1;
        int box_h = box_max_y - box_min_y + 1;

        // Window auf Sub-Rechteck setzen (Display-Koordinaten: + kView-Offset).
        st7789_set_window(kViewX + box_min_x, kViewY + box_min_y, box_w, box_h);

        // RAMWR-Cmd
        gpio_put(PB_PIN_DISP_DC, 0);
        gpio_put(PB_PIN_DISP_CS, 0);
        spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        uint8_t ramwr = 0x2C;
        spi_write_blocking(spi0, &ramwr, 1);

        // Pixel-Phase: 16-bit-Mode
        gpio_put(PB_PIN_DISP_DC, 1);
        spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

        // DMA-Channel einmal konfigurieren, dann pro Zeile retriggern.
        dma_channel_config c = dma_channel_get_default_config(g_dma_chan);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, spi_get_dreq(spi0, true));

        if (box_min_x == 0 && box_w == kViewW) {
            // Sonderfall: volle Zeilenbreite -> Box ist kontiguoes im linearen
            // Buffer, eine einzige DMA reicht.
            dma_channel_configure(
                g_dma_chan, &c,
                &spi_get_hw(spi0)->dr,
                frame + (size_t)box_min_y * kViewW,
                (uint32_t)box_w * box_h,
                true
            );
            dma_channel_wait_for_finish_blocking(g_dma_chan);
        } else {
            // Allgemeiner Fall: pro Zeile eine DMA (Streifen im Buffer).
            for (int y = box_min_y; y <= box_max_y; ++y) {
                const uint16_t *line_ptr = frame + (size_t)y * kViewW + box_min_x;
                dma_channel_configure(
                    g_dma_chan, &c,
                    &spi_get_hw(spi0)->dr,
                    line_ptr,
                    (uint32_t)box_w,
                    true
                );
                dma_channel_wait_for_finish_blocking(g_dma_chan);
            }
        }

        while (spi_is_busy(spi0)) tight_loop_contents();
        gpio_put(PB_PIN_DISP_CS, 1);

        multicore_fifo_push_blocking(1);
    }
}

// Wird einmal in setup() aufgerufen, kickt Core 1 los und reserviert den DMA-Channel.
static void start_display_core() {
    // Beide Frame-Buffer auf 0 setzen. Beim ersten Frame wird sonst gegen
    // SRAM-Muell verglichen -> Dirty-Box waere zufaellig, Rest des Display-
    // bereichs wuerde nie aktualisiert und bliebe schwarz mit Bitresten.
    memset(fb_a, 0, sizeof(fb_a));
    memset(fb_b, 0, sizeof(fb_b));

    g_dma_chan = dma_claim_unused_channel(true);
    multicore_launch_core1(core1_display_worker);
}

// Hellt jeden RGB565-Wert kanalweise um BL_PALETTE_LIFT auf, mit Clipping.
// Wird genutzt um das Dimmen des Backlights optisch zu kompensieren - dunkle
// Toene werden so angehoben dass das Bild trotz weniger Backlight-Strom gut
// lesbar bleibt. Wirkt auf alle drei Layer (BG, OBJ0, OBJ1).
static uint16_t lift_rgb565(uint16_t c, uint8_t lift) {
    // RGB565 -> 8-bit Komponenten (mit den ueblichen Bit-Replikations-Tricks)
    int r = ((c >> 11) & 0x1F) << 3; r |= r >> 5;
    int g = ((c >>  5) & 0x3F) << 2; g |= g >> 6;
    int b = ( c        & 0x1F) << 3; b |= b >> 5;

    r += lift; if (r > 255) r = 255;
    g += lift; if (g > 255) g = 255;
    b += lift; if (b > 255) b = 255;

    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void apply_palette_brightness_lift(uint8_t lift) {
    if (lift == 0) return;
    for (int layer = 0; layer < 3; ++layer) {
        for (int i = 0; i < 4; ++i) {
            palette[layer][i] = lift_rgb565(palette[layer][i], lift);
        }
    }
}

// ---------------------------------------------------------------------------
// Scanline-Output: wird von Peanut-GB pro GB-Zeile (0..143) aufgerufen.
// ---------------------------------------------------------------------------
#if DISPLAY_SCALE_MODE == 0
// 1:1: Zeile in den Frame-Buffer schreiben, dabei pro Pixel mit dem
// vorigen gesendeten Frame vergleichen und Dirty-Box mittracken.
// Am Frame-Ende: Buffer + Box an Core 1 zur Anzeige uebergeben.
extern "C" void __not_in_flash_func(lcd_draw_line)(struct gb_s *, const uint8_t pixels[160], const uint_fast8_t line) {
    if (line == 0) dirty_reset();

    uint16_t *row      = fb_write + (size_t)line * FB_W;
    uint16_t *prev_row = fb_send  + (size_t)line * FB_W;

    int rmin = FB_W, rmax = -1;
    for (uint_fast16_t x = 0; x < FB_W; ++x) {
        uint16_t p = fast_palette[pixels[x]];
        row[x] = p;
        if (p != prev_row[x]) {
            if ((int)x < rmin) rmin = x;
            if ((int)x > rmax) rmax = x;
        }
    }
    dirty_mark_row(line, rmin, rmax);

    if (line == 143) {
        if (g_dma_chan >= 0) {
            // Auf vorherigen Frame warten
            (void)multicore_fifo_pop_blocking();

            // Buffer-Pointer tauschen
            uint16_t *tmp = fb_send;
            fb_send  = fb_write;
            fb_write = tmp;

            // Box vor dem Push lokal sichern (Werte koennen -1 sein).
            uint32_t box = dirty_pack();

            multicore_fifo_push_blocking((uint32_t)fb_send);
            multicore_fifo_push_blocking(box);
        }
    }
}
#else
// 1.25x-Skalierung: 160 -> 200 horizontal, 144 -> 180 vertikal.
// Pattern horizontal (4 in -> 5 out): (a, b, c, c, d) - mittlerer Pixel
// verdoppelt, sieht symmetrischer aus als an den Raendern zu duplizieren.
// Pattern vertikal (4 in -> 5 out): jede 4. Eingangszeile wird verdoppelt.
//
// Beide Patterns nutzen dieselbe Frame-Buffer-Architektur wie SCALE_MODE 0,
// damit Core 1 + DMA weiter den Output schiebt - die Per-Zeilen-SPI-Writes
// vom alten 1.5x-Pfad waren der Bottleneck.
static inline void __not_in_flash_func(scale_line_125_x)(const uint8_t *in, uint16_t *out) {
    // 4 Eingangspixel -> 5 Ausgangspixel, 40 Iterationen, Pattern (a,b,c,c,d).
    // Pointer-Iteration vermeidet 4*i / 5*i Multiplikationen im Index.
    for (int i = 0; i < 40; ++i) {
        uint16_t a = fast_palette[in[0]];
        uint16_t b = fast_palette[in[1]];
        uint16_t c = fast_palette[in[2]];
        uint16_t d = fast_palette[in[3]];
        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = c;
        out[4] = d;
        in  += 4;
        out += 5;
    }
}

// Skaliert eine GB-Zeile auf 200 Output-Pixel UND vergleicht dabei mit
// dem vorigen Frame. Liefert min/max-x der Aenderungen, oder -1 wenn nichts.
static inline void __not_in_flash_func(scale_line_125_x_dirty)(
    const uint8_t *in, uint16_t *out, const uint16_t *prev,
    int *out_min, int *out_max)
{
    int rmin = kViewW, rmax = -1;
    for (int i = 0; i < 40; ++i) {
        uint16_t a = fast_palette[in[0]];
        uint16_t b = fast_palette[in[1]];
        uint16_t c = fast_palette[in[2]];
        uint16_t d = fast_palette[in[3]];
        int base = i * 5;
        out[0] = a;  if (a != prev[0]) { if (base+0 < rmin) rmin = base+0; if (base+0 > rmax) rmax = base+0; }
        out[1] = b;  if (b != prev[1]) { if (base+1 < rmin) rmin = base+1; if (base+1 > rmax) rmax = base+1; }
        out[2] = c;  if (c != prev[2]) { if (base+2 < rmin) rmin = base+2; if (base+2 > rmax) rmax = base+2; }
        out[3] = c;  if (c != prev[3]) { if (base+3 < rmin) rmin = base+3; if (base+3 > rmax) rmax = base+3; }
        out[4] = d;  if (d != prev[4]) { if (base+4 < rmin) rmin = base+4; if (base+4 > rmax) rmax = base+4; }
        in  += 4;
        out += 5;
        prev += 5;
    }
    *out_min = rmin;
    *out_max = rmax;
}

extern "C" void __not_in_flash_func(lcd_draw_line)(struct gb_s *, const uint8_t pixels[160], const uint_fast8_t line) {
    if (line == 0) dirty_reset();

    // Output-y berechnen.
    int q = line >> 2;
    int r = line & 3;
    int y_base, y_count;
    switch (r) {
        case 0: y_base = q * 5 + 0; y_count = 1; break;
        case 1: y_base = q * 5 + 1; y_count = 1; break;
        case 2: y_base = q * 5 + 2; y_count = 2; break;
        default: y_base = q * 5 + 4; y_count = 1; break;
    }

    uint16_t *first_row = fb_write + (size_t)y_base * kViewW;
    uint16_t *prev_row  = fb_send  + (size_t)y_base * kViewW;
    int rmin, rmax;
    scale_line_125_x_dirty(pixels, first_row, prev_row, &rmin, &rmax);
    dirty_mark_row(y_base, rmin, rmax);

    if (y_count == 2) {
        // Zweite Output-Zeile schreiben + extra vergleichen (gleiche Pixel,
        // aber prev koennte sich von erster Zeile unterscheiden -> separat tracken).
        uint16_t *second_row     = first_row + kViewW;
        uint16_t *second_prev    = prev_row  + kViewW;
        int rmin2 = kViewW, rmax2 = -1;
        for (int x = 0; x < kViewW; ++x) {
            uint16_t p = first_row[x];
            second_row[x] = p;
            if (p != second_prev[x]) {
                if (x < rmin2) rmin2 = x;
                if (x > rmax2) rmax2 = x;
            }
        }
        dirty_mark_row(y_base + 1, rmin2, rmax2);
    }

    if (line == 143) {
        if (g_dma_chan >= 0) {
            (void)multicore_fifo_pop_blocking();
            uint16_t *tmp = fb_send;
            fb_send  = fb_write;
            fb_write = tmp;

            uint32_t box = dirty_pack();

            multicore_fifo_push_blocking((uint32_t)fb_send);
            multicore_fifo_push_blocking(box);
        }
    }
}
#endif

// ===========================================================================
// Input
// ===========================================================================
// Mapping:
//   D-Pad        = UP/DOWN/LEFT/RIGHT
//   GB A / B     = A / B
//   GB START     = LEFT + RIGHT gleichzeitig  (oder externe Taste GP1)
//   GB SELECT    = UP + DOWN gleichzeitig     (oder externe Taste GP0)
// Auf einem echten Game Boy koennen gegenueberliegende Richtungen nie
// gleichzeitig gedrueckt sein, die Kombinationen kollidieren also mit
// keinem Spiel. Solange eine Kombination aktiv war, werden die beiden
// Richtungen unterdrueckt, bis BEIDE wieder losgelassen sind - sonst
// wuerde beim Loslassen kurz eine einzelne Richtung im Spiel ankommen.

#if PB_HAS_CENTER
// PicoBoy Color: CENTER ist ein Modifier - allein gedrueckt passiert nichts.
// Solange CENTER unten ist, liefern A/B nicht GB-A/GB-B, sondern SELECT/START.
static PbInput read_buttons() {
    PbInput s;
    s.dpad_up    = !gpio_get(PB_PIN_JOY_UP);
    s.dpad_down  = !gpio_get(PB_PIN_JOY_DOWN);
    s.dpad_left  = !gpio_get(PB_PIN_JOY_LEFT);
    s.dpad_right = !gpio_get(PB_PIN_JOY_RIGHT);
    bool a_raw   = !gpio_get(PB_PIN_BTN_A);
    bool b_raw   = !gpio_get(PB_PIN_BTN_B);
    bool ctr     = !gpio_get(PB_PIN_JOY_CENTER);
#if PB_SWAP_AB
    s.btn_a      = b_raw && !ctr;   // Taste B = GB-A
    s.btn_b      = a_raw && !ctr;   // Taste A = GB-B
#else
    s.btn_a      = a_raw && !ctr;
    s.btn_b      = b_raw && !ctr;
#endif
    s.btn_select = a_raw && ctr;
    s.btn_start  = b_raw && ctr;
    return s;
}
#else
static PbInput read_buttons() {
    static bool lr_chord = false;   // LEFT+RIGHT war gemeinsam unten
    static bool ud_chord = false;   // UP+DOWN war gemeinsam unten

    bool up    = !gpio_get(PB_PIN_JOY_UP);
    bool down  = !gpio_get(PB_PIN_JOY_DOWN);
    bool left  = !gpio_get(PB_PIN_JOY_LEFT);
    bool right = !gpio_get(PB_PIN_JOY_RIGHT);

    if (left && right)  lr_chord = true;
    if (!left && !right) lr_chord = false;
    if (up && down)     ud_chord = true;
    if (!up && !down)   ud_chord = false;

    PbInput s;
    s.dpad_up    = up    && !ud_chord;
    s.dpad_down  = down  && !ud_chord;
    s.dpad_left  = left  && !lr_chord;
    s.dpad_right = right && !lr_chord;
    s.btn_a      = !gpio_get(PB_PIN_BTN_A);
    s.btn_b      = !gpio_get(PB_PIN_BTN_B);
    s.btn_start  = (left && right) || !gpio_get(PB_PIN_BTN_START);
    s.btn_select = (up && down)    || !gpio_get(PB_PIN_BTN_SELECT);
    return s;
}
#endif

// ===========================================================================
// Save in Flash
// ---------------------------------------------------------------------------
// Save-Region: letzte 64 KB der 2-MB-Flash. Header (Magic + ROM-CRC + Groesse)
// am Anfang der Region, danach Cart-RAM. Schreiben nur explizit auf Hotkey.
// ===========================================================================
#define SAVE_FLASH_OFFSET   PBGB_SAVE_FLASH_OFFSET             // 0x1F0000
#define SAVE_MAGIC          0x50424753u                        // "PBGS"

struct __attribute__((packed)) SaveHeader {
    uint32_t magic;
    uint32_t rom_hash;     // einfacher Hash zur ROM-Zuordnung
    uint32_t save_size;
    uint32_t reserved;
};

static uint32_t simple_rom_hash(const uint8_t *rom, uint32_t len) {
    // FNV-1a ueber die ersten 4 KB - genug zur Spielzuordnung
    uint32_t h = 2166136261u;
    uint32_t n = len < 4096 ? len : 4096;
    for (uint32_t i = 0; i < n; ++i) {
        h ^= rom[i];
        h *= 16777619u;
    }
    return h;
}

// ACHTUNG: diese Funktion MUSS aus dem RAM laufen, sonst haengt sie sich auf,
// wenn der XIP-Cache invalidiert wird. arduino-pico legt Funktionen normalerweise
// in den Flash; wir markieren sie explizit.
static void __no_inline_not_in_flash_func(write_save)(const uint8_t *data, uint32_t len) {
    SaveHeader hdr;
    hdr.magic = SAVE_MAGIC;
    hdr.rom_hash = simple_rom_hash(active_rom, active_rom_len);
    hdr.save_size = len;
    hdr.reserved = 0;

    uint8_t page[FLASH_PAGE_SIZE];   // 256
    // Erste Page: Header + bis zu 240 Bytes Save-Daten
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &hdr, sizeof(hdr));
    uint32_t copy = len < (FLASH_PAGE_SIZE - sizeof(hdr)) ? len : (FLASH_PAGE_SIZE - sizeof(hdr));
    memcpy(page + sizeof(hdr), data, copy);

    uint32_t intr = save_and_disable_interrupts();
    // Erst loeschen (sektorweise = 4 KB), dann schreiben (pageweise = 256 B).
    flash_range_erase(SAVE_FLASH_OFFSET, PBGB_SAVE_FLASH_SIZE);
    flash_range_program(SAVE_FLASH_OFFSET, page, FLASH_PAGE_SIZE);

    uint32_t written = copy;
    while (written < len) {
        memset(page, 0xFF, sizeof(page));
        uint32_t block = (len - written) < FLASH_PAGE_SIZE ? (len - written) : FLASH_PAGE_SIZE;
        memcpy(page, data + written, block);
        flash_range_program(SAVE_FLASH_OFFSET + sizeof(hdr) + written, page, FLASH_PAGE_SIZE);
        written += block;
    }
    restore_interrupts(intr);
}

static bool read_save(uint8_t *data, uint32_t len) {
    const SaveHeader *hdr = (const SaveHeader *)(XIP_BASE + SAVE_FLASH_OFFSET);
    if (hdr->magic != SAVE_MAGIC) return false;
    if (hdr->rom_hash != simple_rom_hash(active_rom, active_rom_len)) return false;
    if (hdr->save_size != len) return false;
    const uint8_t *src = (const uint8_t *)(XIP_BASE + SAVE_FLASH_OFFSET + sizeof(SaveHeader));
    memcpy(data, src, len);
    return true;
}

// ===========================================================================
// USB-Stick-Modus: FatFS-Partition als USB-Massenspeicher exportieren
// ===========================================================================
//
// Aktivierung: B halten und RESET druecken (oder einschalten).
// Modus-Wechsel zurueck: RESET ohne B -> Spielmodus.
//
// Constraint von arduino-pico FatFSUSB: PC-Zugriff und Pico-Zugriff sind
// exklusiv. Wir machen es einfach: in diesem Modus wird der Emulator
// nicht gestartet, das Display zeigt nur den USB-Stick-Hinweis und das
// FatFS gehoert dem Host. Modus-Wechsel ueber RESET (kein Race-Risiko).
// ===========================================================================

// Schreibt eine Welcome-Datei ins FatFS-Root, falls noch keine da ist.
static void fatfs_ensure_welcome() {
    FILINFO fno;
    if (f_stat("/README.TXT", &fno) == FR_OK) return;   // already present
    FIL f;
    if (f_open(&f, "/README.TXT", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    f_puts("PicoBoyGB - Game Boy DMG Emulator\r\n", &f);
    f_puts("\r\n", &f);
    f_puts("Drag .gb ROMs onto this drive, then EJECT it.\r\n", &f);
    f_puts("Press RESET (without holding " PB_TXT_USB_KEY ") to return\r\n", &f);
    f_puts("to the game menu.\r\n", &f);
    f_puts("\r\n", &f);
    f_puts("Hold " PB_TXT_USB_KEY " while pressing RESET to come back\r\n", &f);
    f_puts("to this USB stick mode.\r\n", &f);
    f_puts("\r\n", &f);
    f_puts("In the menu use UP/DOWN to navigate, A or " PB_TXT_OK_KEY "\r\n", &f);
    f_puts("to confirm.\r\n", &f);
    f_close(&f);
}

// Sammelt alle .gb-Files im FatFS-Root in eine Liste.
// Rueckgabe: Anzahl gefundener Files (max MAX_ROMS).
#define MAX_ROMS         32
#define MAX_ROMNAME_LEN  64

static char     g_rom_names[MAX_ROMS][MAX_ROMNAME_LEN];
static int      g_rom_count = 0;

static int strcmp_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
        if (ca != cb) return ca - cb;
        ++a; ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int fatfs_scan_gb_files() {
    g_rom_count = 0;
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, "/") != FR_OK) return 0;
    while (g_rom_count < MAX_ROMS &&
           f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & (AM_DIR | AM_SYS | AM_HID)) continue;
        const char *name = fno.fname;
        size_t L = strlen(name);
        if (L < 4) continue;
        // Endet auf .gb (case-insensitive)?
        if (strcasecmp(name + L - 3, ".gb") != 0) continue;
        strncpy(g_rom_names[g_rom_count], name, MAX_ROMNAME_LEN);
        g_rom_names[g_rom_count][MAX_ROMNAME_LEN - 1] = 0;
        ++g_rom_count;
    }
    f_closedir(&dir);
    // Alphabetisch sortieren (case-insensitive)
    for (int i = 1; i < g_rom_count; ++i) {
        for (int j = i; j > 0 && strcmp_ci(g_rom_names[j], g_rom_names[j-1]) < 0; --j) {
            char tmp[MAX_ROMNAME_LEN];
            memcpy(tmp, g_rom_names[j],   MAX_ROMNAME_LEN);
            memcpy(g_rom_names[j],   g_rom_names[j-1], MAX_ROMNAME_LEN);
            memcpy(g_rom_names[j-1], tmp,             MAX_ROMNAME_LEN);
        }
    }
    return g_rom_count;
}

// Game-Boy-Screen-Geometrie fuer Menue und Meldungen: 160x144 zentriert.
// Frueh definiert, damit alle Status-Screens (Menu, Loading, USB-Mode,
// Controls-Splash) den gleichen Bereich nutzen wie spaeter der Emulator.
#define GB_VIEW_X   ((PB_DISP_W - 160) / 2)   // 40
#define GB_VIEW_Y   ((PB_DISP_H - 144) / 2)   // 88
#define GB_VIEW_W   160
#define GB_VIEW_H   144

// Liefert die 4 Farben der aktiven DMG-Palette (Background-Slot).
// palette[0][0] = hellste, palette[0][3] = dunkelste. Wird ggf. ueber
// rebuild_fast_palette() gesetzt - vorher liefern wir Default-DMG-Gruen.
static uint16_t palette_color(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    static const uint16_t default_dmg[4] = {
        0x9D6A,  // hellstes Gruen
        0x8528,  // helles Gruen
        0x32C7,  // dunkles Gruen
        0x0801,  // dunkelstes Gruen
    };
    uint16_t c = palette[0][idx];
    return c ? c : default_dmg[idx];
}

// ---------------------------------------------------------------------------
// Rahmen im Stil des ersten Game Boy: das Grau der Scheibe fuellt den ganzen
// Bildschirm, ueber dem Display zwei Streifen (weinrot und dunkelblau), in
// der Mitte unterbrochen von "DOT MATRIX". Ohne Batterie-LED und ohne
// Schriftzug. Das Spielbild (kViewX/kViewY, 200x180) liegt darin; vor dem
// Spiel ist es mit der hellsten Palettenfarbe gefuellt (wie ein leeres LCD).
// Die Streifen sitzen wie auf einer Scheibe, die 14 px ueber das Bild
// hinausragt und 44 px darueber beginnt.
// ---------------------------------------------------------------------------
static const int kLensX = kViewX - 14;                 //   6
static const int kLensY = kViewY - 44;                 //  26 (PBC: 6)
static const int kLensW = kViewW + 28;                 // 228

static void draw_bezel() {
    const uint16_t c_lens   = st7789_color565( 92,  94, 108);   // Scheibe
    const uint16_t c_red    = st7789_color565(142,  34,  82);   // oberer Streifen
    const uint16_t c_blue   = st7789_color565( 44,  48, 132);   // unterer Streifen
    const uint16_t c_text   = st7789_color565(196, 198, 208);

    st7789_fill(c_lens);

    // Zwei Streifen ueber dem Display, unterbrochen von "DOT MATRIX"
    const char *label = "DOT MATRIX";
    const int text_w  = (int)strlen(label) * 8;
    const int text_x  = kLensX + (kLensW - text_w) / 2;
    const int text_y  = kLensY + 15;                       // 8 px hoch
    const int line_x0 = kLensX + 10;
    const int line_x1 = kLensX + kLensW - 10;              // exklusiv
    const int gap     = 6;
    const int y_red   = text_y + 1;
    const int y_blue  = text_y + 5;
    st7789_fill_rect(line_x0, y_red,  text_x - gap - line_x0, 2, c_red);
    st7789_fill_rect(line_x0, y_blue, text_x - gap - line_x0, 2, c_blue);
    st7789_fill_rect(text_x + text_w + gap, y_red,  line_x1 - (text_x + text_w + gap), 2, c_red);
    st7789_fill_rect(text_x + text_w + gap, y_blue, line_x1 - (text_x + text_w + gap), 2, c_blue);
    st7789_draw_text(text_x, text_y, label, c_text, c_lens, 1);

    // Display-Flaeche wie ein leeres LCD
    st7789_fill_rect(kViewX, kViewY, kViewW, kViewH, palette_color(0));
}

// Zeichnet einen GB-Screen-Bereich mit Palette-Hintergrund + zentriertem Text.
// Drumherum der Game-Boy-Rahmen, der Bereich selbst nutzt die GB-Palette.
static void gb_screen_message(const char *line1, const char *line2, const char *line3) {
    draw_bezel();
    st7789_fill_rect(GB_VIEW_X, GB_VIEW_Y, GB_VIEW_W, GB_VIEW_H, palette_color(0));

    // 2-Pixel-Rahmen in palette[0][3] (dunkel)
    st7789_fill_rect(GB_VIEW_X,                     GB_VIEW_Y,                  GB_VIEW_W, 2, palette_color(3));
    st7789_fill_rect(GB_VIEW_X,                     GB_VIEW_Y + GB_VIEW_H - 2,  GB_VIEW_W, 2, palette_color(3));
    st7789_fill_rect(GB_VIEW_X,                     GB_VIEW_Y,                  2, GB_VIEW_H, palette_color(3));
    st7789_fill_rect(GB_VIEW_X + GB_VIEW_W - 2,     GB_VIEW_Y,                  2, GB_VIEW_H, palette_color(3));

    uint16_t bg = palette_color(0);
    uint16_t fg = palette_color(3);

    int total = 0;
    if (line1) total += 16;
    if (line2) total += 16;
    if (line3) total += 16;
    int y = GB_VIEW_Y + (GB_VIEW_H - total) / 2;

    auto centered = [&](const char *s) {
        if (!s) return;
        int w = (int)strlen(s) * 8;
        int x = GB_VIEW_X + (GB_VIEW_W - w) / 2;
        if (x < GB_VIEW_X + 4) x = GB_VIEW_X + 4;
        st7789_draw_text(x, y, s, fg, bg, 1);
        y += 16;
    };
    centered(line1);
    centered(line2);
    centered(line3);
}

// Truncates filename to fit one line in the GB area (max 18 chars).
static void truncate_name(const char *src, char *dst, size_t max_chars, size_t dst_size) {
    size_t lim = (max_chars < dst_size - 1) ? max_chars : dst_size - 1;
    size_t i = 0;
    while (src[i] && i < lim) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

// Zeichnet einen 2-Pixel-Rahmen in palette[0][3] um den GB-Bereich.
static void gb_draw_border() {
    uint16_t fg = palette_color(3);
    st7789_fill_rect(GB_VIEW_X,                 GB_VIEW_Y,                 GB_VIEW_W, 2, fg);
    st7789_fill_rect(GB_VIEW_X,                 GB_VIEW_Y + GB_VIEW_H - 2, GB_VIEW_W, 2, fg);
    st7789_fill_rect(GB_VIEW_X,                 GB_VIEW_Y,                 2, GB_VIEW_H, fg);
    st7789_fill_rect(GB_VIEW_X + GB_VIEW_W - 2, GB_VIEW_Y,                 2, GB_VIEW_H, fg);
}

// "How to upload"-Bildschirm. Blockiert bis A, B oder START gedrueckt wird.
static void show_info_screen() {
    uint16_t bg = palette_color(0);
    uint16_t fg = palette_color(3);

    // Layout passt zum Menue-Raster: 12 Zeilen a 11 px innerhalb des
    // 160x144-GB-Bereichs. Damit der Wechsel vom Menue NICHT flackert,
    // fluten wir den Bereich nicht mit Hintergrund - stattdessen werden
    // die Menue-Zeilen Stueck fuer Stueck mit dem Info-Inhalt ueberschrieben
    // (jede Zeile: Streifen clearen + Text drauf, in einem Rutsch).
    const int row_h = 11;
    const char *rows[12] = {
        "HOW TO UPLOAD",
        "Hold " PB_TXT_USB_KEY " and",
        "press RESET.",
        "Drop .gb files,",
        "eject the drive,",
        "press RESET again",
        "WITHOUT " PB_TXT_USB_KEY ".",
        "",
        "IN GAME:",
        PB_TXT_START " = START",
        PB_TXT_SELECT " = SELECT",
        "Press A to back",
    };

    int x = GB_VIEW_X + 6;
    for (int r = 0; r < 12; ++r) {
        int y = GB_VIEW_Y + 2 + r * row_h;
        // Zeile auf hellen Hintergrund setzen (ueberschreibt evtl. invertierte
        // Menue-Selektion oder alten Text in einem Rutsch).
        st7789_fill_rect(GB_VIEW_X + 2, y, GB_VIEW_W - 4, row_h, bg);
        if (rows[r][0]) {
            st7789_draw_text(x, y + 2, rows[r], fg, bg, 1);
        }
    }

    // Rahmen am Ende neu malen (falls die Streifen-Clears die 2px-Border
    // angekratzt haben - sind sie nicht, da Clear bei x+2 startet, aber
    // sicher ist sicher und kostet kaum etwas).
    gb_draw_border();

    // Edge-Detect: warten auf Press, dann auf Release. A, B oder START
    // werden alle als "zurueck" akzeptiert.
    auto any_confirm = []() {
        return (!gpio_get(PB_PIN_BTN_A))
            || (!gpio_get(PB_PIN_BTN_B))
            || (!gpio_get(PB_PIN_MENU_OK));
    };
    bool prev = any_confirm();
    while (true) {
        bool now = any_confirm();
        if (now && !prev) break;
        prev = now;
        delay(20);
    }
    while (any_confirm()) delay(20);
}

// Result-Codes von run_main_menu()
#define MENU_RESULT_NONE   -1   // wird intern nicht geliefert; Sentinel

// Zeichnet eine einzelne Menue-Zeile (loescht Hintergrund + zeichnet Text).
// row_idx ist die sichtbare Zeile (0..visible_rows-1), item der globale Index.
static void draw_menu_row(int item, int row_idx, bool is_sel, int row_h) {
    uint16_t bg = palette_color(0);
    uint16_t fg = palette_color(3);
    uint16_t row_bg = is_sel ? fg : bg;
    uint16_t row_fg = is_sel ? bg : fg;

    int y = GB_VIEW_Y + 2 + row_idx * row_h;

    // Zeile clearen (innerhalb des 2px-Rahmens)
    st7789_fill_rect(GB_VIEW_X + 2, y, GB_VIEW_W - 4, row_h, row_bg);

    int tx = GB_VIEW_X + 6;
    if (item == 0) {
        st7789_draw_text(tx, y + 2, "Info", row_fg, row_bg, 1);
    } else if (item == MENU_AUDIO_ITEM) {
        st7789_draw_text(tx, y + 2, "Audio:", row_fg, row_bg, 1);
        st7789_draw_text(tx + 8 * 7, y + 2,
                         kAudioNames[g_audio_level],
                         row_fg, row_bg, 1);
    } else {
        int file_idx = item - MENU_SPECIAL_COUNT;
        char shown[20];
        truncate_name(g_rom_names[file_idx], shown, 18, sizeof(shown));
        st7789_draw_text(tx, y + 2, shown, row_fg, row_bg, 1);
    }
}

// Hauptmenue: Info, Audio (nur mit Ton, LEFT/RIGHT toggle), dann .gb-Files.
// Rueckgabe: Index >= 0 in g_rom_names[] des gewaehlten Files.
static int run_main_menu() {
    uint16_t bg = palette_color(0);
    uint16_t fg = palette_color(3);

    const int SPECIAL_COUNT = MENU_SPECIAL_COUNT;
    int total_items = SPECIAL_COUNT + g_rom_count;

    int sel = (g_rom_count > 0) ? SPECIAL_COUNT : 0;
    int top = 0;
    int prev_sel = -1;     // -1 = noch nichts gezeichnet
    int prev_top = -1;
    int prev_audio = g_audio_level;
    bool prev_up = false, prev_dn = false, prev_lt = false, prev_rt = false, prev_a = false;

    // Initial-Debounce
    uint32_t t0 = millis();
    while (millis() - t0 < 100) {
        if (!gpio_get(PB_PIN_BTN_A)     ||
            !gpio_get(PB_PIN_BTN_B)     ||
            !gpio_get(PB_PIN_MENU_OK)   ||
            !gpio_get(PB_PIN_JOY_UP)    ||
            !gpio_get(PB_PIN_JOY_DOWN)  ||
            !gpio_get(PB_PIN_JOY_LEFT)  ||
            !gpio_get(PB_PIN_JOY_RIGHT))
        {
            t0 = millis();
        }
        delay(5);
    }

    const int row_h        = 11;
    const int content_h    = GB_VIEW_H - 4;
    const int visible_rows = content_h / row_h;     // 12

    // Helper: kompletten Menue-Hintergrund + alle Zeilen zeichnen
    auto draw_full = [&]() {
        // KEIN Vollbild-Black-Fill - waere sichtbares Flackern. Nur GB-Bereich.
        st7789_fill_rect(GB_VIEW_X, GB_VIEW_Y, GB_VIEW_W, GB_VIEW_H, bg);
        gb_draw_border();
        for (int r = 0; r < visible_rows; ++r) {
            int item = top + r;
            if (item >= total_items) break;
            draw_menu_row(item, r, item == sel, row_h);
        }
        // Scroll-Indikator
        if (total_items > visible_rows) {
            int track_y = GB_VIEW_Y + 2;
            int track_h = visible_rows * row_h;
            int bar_h   = (visible_rows * track_h) / total_items;
            if (bar_h < 4) bar_h = 4;
            int bar_y   = track_y + (top * track_h) / total_items;
            st7789_fill_rect(GB_VIEW_X + GB_VIEW_W - 4, bar_y, 2, bar_h, fg);
        }
    };

    while (true) {
        // Scroll-Anpassung
        if (sel < top) top = sel;
        if (sel >= top + visible_rows) top = sel - visible_rows + 1;
        if (top < 0) top = 0;

        // Redraw-Strategie: voll bei initial/scroll/return-from-Info,
        // sonst nur die zwei betroffenen Zeilen.
        if (prev_sel < 0 || top != prev_top) {
            draw_full();
        } else if (sel != prev_sel) {
            // Alte Zeile entheben
            if (prev_sel >= top && prev_sel < top + visible_rows) {
                draw_menu_row(prev_sel, prev_sel - top, false, row_h);
            }
            // Neue Zeile hervorheben
            if (sel >= top && sel < top + visible_rows) {
                draw_menu_row(sel, sel - top, true, row_h);
            }
        } else if (prev_audio != g_audio_level && sel == MENU_AUDIO_ITEM) {
            // Nur Audio-Zeile neu malen (Toggle ohne Selektionswechsel)
            if (sel >= top && sel < top + visible_rows) {
                draw_menu_row(sel, sel - top, true, row_h);
            }
        }
        prev_sel = sel;
        prev_top = top;
        prev_audio = g_audio_level;

        bool up = !gpio_get(PB_PIN_JOY_UP);
        bool dn = !gpio_get(PB_PIN_JOY_DOWN);
        bool lt = !gpio_get(PB_PIN_JOY_LEFT);
        bool rt = !gpio_get(PB_PIN_JOY_RIGHT);
        bool a  = !gpio_get(PB_PIN_BTN_A);
        bool b  = !gpio_get(PB_PIN_BTN_B);
        bool st = !gpio_get(PB_PIN_MENU_OK);
        bool confirm = a || b || st;

        if (up && !prev_up && sel > 0) { sel--; }
        if (dn && !prev_dn && sel < total_items - 1) { sel++; }

        if (sel == MENU_AUDIO_ITEM) {
            if (rt && !prev_rt) g_audio_level = (g_audio_level + 1) % (AUDIO_LEVELS + 1);
            if (lt && !prev_lt) g_audio_level = (g_audio_level + AUDIO_LEVELS) % (AUDIO_LEVELS + 1);
            g_audio_enabled = g_audio_level > 0;
        }

        if (confirm && !prev_a) {
            if (sel == 0) {
                show_info_screen();
                prev_sel = -1;   // Info hat den Bildschirm uebermalt -> full redraw
            } else if (sel == MENU_AUDIO_ITEM) {
                // Audio: A/B/START ohne Funktion (nur LEFT/RIGHT togglet)
            } else {
                return sel - SPECIAL_COUNT;
            }
        }

        prev_up = up; prev_dn = dn; prev_lt = lt; prev_rt = rt; prev_a = confirm;
        delay(20);
    }
}

// Zeigt einen Fehlerbildschirm und blockiert, bis Power-Cycle.
static void show_fatal_error(const char *kind, const char *detail) {
    st7789_fill(0x0000);
    st7789_draw_text(8,  28, kind,   st7789_color565(255, 100, 100), 0x0000, 2);
    st7789_draw_text(8,  60, detail, 0xFFFF, 0x0000, 1);
    st7789_draw_text(8, 100, "Power-cycle to retry.", st7789_color565(180, 180, 180), 0x0000, 1);
    while (true) tight_loop_contents();
}

// ===========================================================================
// Absturzanzeige: Bei einem HardFault (beide Kerne) zeigt der Bildschirm
// "CRASH" mit Kern, PC und LR und bleibt stehen. Zuordnen mit
//   arm-none-eabi-addr2line -e build/<firmware>.elf <Adresse>
// ===========================================================================
extern "C" void __not_in_flash_func(hardfault_report)(uint32_t *frame) {
    uint32_t pc = frame[6];
    uint32_t lr = frame[5];
    uint core = get_core_num();
    if (core == 0) multicore_reset_core1();   // Kern 1 vom SPI wegholen
    char line[32];
    st7789_fill_rect(0, 0, PB_DISP_W, 60, 0x0000);
    snprintf(line, sizeof(line), "CRASH core %u", core);
    st7789_draw_text(8, 8, line, st7789_color565(255, 90, 90), 0x0000, 2);
    snprintf(line, sizeof(line), "PC %08lX", (unsigned long)pc);
    st7789_draw_text(8, 30, line, 0xFFFF, 0x0000, 1);
    snprintf(line, sizeof(line), "LR %08lX", (unsigned long)lr);
    st7789_draw_text(8, 42, line, 0xFFFF, 0x0000, 1);
    while (true) tight_loop_contents();
}

// Ersetzt den schwachen isr_hardfault des SDK (dort nur "bkpt").
extern "C" __attribute__((naked)) void isr_hardfault(void) {
    __asm volatile(
        "movs r0, #4          \n"
        "mov  r1, lr          \n"
        "tst  r0, r1          \n"
        "beq  1f              \n"
        "mrs  r0, psp         \n"
        "b    2f              \n"
        "1:                   \n"
        "mrs  r0, msp         \n"
        "2:                   \n"
        "ldr  r1, =hardfault_report \n"
        "bx   r1              \n");
}

// ===========================================================================
// ROM direkt aus der Datei im Flash
// ---------------------------------------------------------------------------
// Ein FatFs-Sektor ist ein 4-KB-Flash-Sektor (flash_fs.h). Liegt die Datei in
// einem einzigen Stueck (Cluster-Kette ohne Luecke), steht das ROM also
// linear im Flash und kann per XIP gelesen werden, ohne Kopie.
// ===========================================================================

// Flash-Adresse der Datei, wenn sie zusammenhaengend ist, sonst nullptr.
static const uint8_t *fatfs_contiguous_ptr(FIL *f) {
    DWORD clmt[8];
    clmt[0] = sizeof(clmt) / sizeof(clmt[0]);
    f->cltbl = clmt;
    FRESULT r = f_lseek(f, CREATE_LINKMAP);
    f->cltbl = nullptr;
    // Ein Fragment: {Tabellengroesse = 4, Clusterzahl, Startcluster, 0}.
    if (r != FR_OK || clmt[0] != 4) return nullptr;
    FATFS *fs = f->obj.fs;
    LBA_t sect = fs->database + (LBA_t)fs->csize * (clmt[2] - 2);
    return (const uint8_t *)(XIP_BASE + PBGB_FS_FLASH_OFFSET
                             + (uint32_t)sect * PBGB_FS_SECTOR_SIZE);
}

// Schreibt die Datei in einen zusammenhaengenden Bereich um (f_expand), mit
// buf (4 KB) als Zwischenpuffer. Falsch, wenn kein freies Stueck gross genug ist.
static bool fatfs_make_contiguous(const char *path, FSIZE_t size, uint8_t *buf) {
    static const char tmp_path[] = "/~PBGB.TMP";
    FIL src, dst;
    f_unlink(tmp_path);   // Rest eines abgebrochenen Laufs
    if (f_open(&dst, tmp_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    if (f_expand(&dst, size, 1) != FR_OK) {
        f_close(&dst);
        f_unlink(tmp_path);
        return false;
    }
    if (f_open(&src, path, FA_READ) != FR_OK) {
        f_close(&dst);
        f_unlink(tmp_path);
        return false;
    }
    bool ok = true;
    for (FSIZE_t done = 0; ok && done < size; ) {
        UINT want = (size - done) > PBGB_FS_SECTOR_SIZE ? PBGB_FS_SECTOR_SIZE : (UINT)(size - done);
        UINT got = 0, put = 0;
        ok = f_read(&src, buf, want, &got) == FR_OK && got == want
          && f_write(&dst, buf, want, &put) == FR_OK && put == want;
        done += want;
    }
    f_close(&src);
    if (f_close(&dst) != FR_OK) ok = false;
    if (!ok) {
        f_unlink(tmp_path);
        return false;
    }
    // Erst das Original weg, dann umbenennen (f_rename verlangt freien Namen).
    return f_unlink(path) == FR_OK && f_rename(tmp_path, path) == FR_OK;
}

// Setzt active_rom/active_rom_len auf das gewaehlte Spiel. Verlangt
// gemountetes FatFS. buf ist ein 4-KB-Puffer fuer ein eventuelles Umkopieren.
static void fatfs_select_rom(const char *name, uint8_t *buf) {
    char path[2 + MAX_ROMNAME_LEN];
    snprintf(path, sizeof(path), "/%s", name);

    char shortname[20];
    strncpy(shortname, name, sizeof(shortname));
    shortname[sizeof(shortname)-1] = 0;

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) show_fatal_error("OPEN FAILED:", name);
    FSIZE_t sz = f_size(&f);
    if (sz < 0x150 || sz > ROM_MAX_SIZE) {
        f_close(&f);
        show_fatal_error("BAD SIZE:", name);
    }
    const uint8_t *rom = fatfs_contiguous_ptr(&f);
    f_close(&f);

    if (!rom) {
        // Fragmentiert (z. B. nach Loeschen und neu Kopieren auf dem PC):
        // einmal zusammenhaengend umschreiben, danach startet es sofort.
        gb_screen_message("DEFRAGMENTING", shortname, "please wait");
        if (!fatfs_make_contiguous(path, sz, buf)) {
            gb_screen_message("NOT ENOUGH ROOM", "delete files and", "copy game again");
            while (true) tight_loop_contents();
        }
        if (f_open(&f, path, FA_READ) != FR_OK) show_fatal_error("OPEN FAILED:", name);
        rom = fatfs_contiguous_ptr(&f);
        f_close(&f);
        if (!rom) show_fatal_error("DEFRAG FAILED:", name);
    }

    if (!rom_looks_valid(rom)) show_fatal_error("NOT A GB ROM:", name);

    active_rom = rom;
    // Header-Byte 0x148 codiert die Groesse (32 KB << val); nie ueber das
    // Dateiende hinaus lesen, dahinter liegen andere Dateien.
    uint8_t code = rom[0x148];
    uint32_t hdr_len = code <= 8 ? (uint32_t)32768 << code : (uint32_t)sz;
    active_rom_len = hdr_len < (uint32_t)sz ? hdr_len : (uint32_t)sz;
}

// Endlos-Modus, der das Drive an den Host exportiert.
static void run_usb_stick_mode() {
    // Display: USB-Stick-Hinweis im GB-Bildschirm-Bereich, mit GB-Palette.
    uint16_t bg = palette_color(0);
    uint16_t fg = palette_color(3);

    draw_bezel();
    st7789_fill_rect(GB_VIEW_X, GB_VIEW_Y, GB_VIEW_W, GB_VIEW_H, bg);
    gb_draw_border();

    int x = GB_VIEW_X + 6;
    int y = GB_VIEW_Y + 6;
    auto line = [&](const char *s) {
        st7789_draw_text(x, y, s, fg, bg, 1);
        y += 11;
    };

    line("USB STICK MODE");
    y += 4;
    line("Now drop .gb files");
    line("onto the PICOBOYGB");
    line("drive, then EJECT");
    line("it on your PC.");
    y += 4;
    line("After eject:");
    line("press RESET");
    line("WITHOUT holding");
    line(PB_TXT_USB_KEY " to play.");

    // FatFs einmal mounten um Welcome-Datei sicherzustellen, dann wieder
    // freigeben damit der Host (per USB-MSC) vollen Zugriff bekommt. Beim
    // allerersten Start ist die Region leer -> automatisch formatieren.
    bool fs_ok = fs_mount();
    if (!fs_ok) {
        if (fs_format()) fs_ok = fs_mount();
    }
    if (fs_ok) {
        fatfs_ensure_welcome();
        fs_unmount();          // Medium dem Host ueberlassen (kein Doppelzugriff)
        tud_init(BOARD_TUD_RHPORT);   // USB-Device-Stack starten (MSC)
    } else {
        gb_screen_message("FS FORMAT FAILED", "flash region", "unwritable?");
        while (true) tight_loop_contents();
    }

    // USB endlos bedienen. Der Host liest/schreibt die Flash-Partition ueber
    // die MSC-Callbacks (usb_msc.c). Verlassen wird der Modus per RESET.
    while (true) {
        tud_task();
    }
}

// ===========================================================================
// Setup
// ===========================================================================

void setup() {
#if ENABLE_VREG_BOOST
    // Core-Spannung HOEHER setzen, BEVOR der Takt hochgedreht wird. Der
    // interne Regulator braucht etwas Zeit zum Settling, damit die neue
    // Spannung anliegt bevor die PLL beim hoeheren Takt mehr Saft braucht.
    vreg_set_voltage(VREG_BOOST_LEVEL);
    busy_wait_us(1000);   // 1 ms Settling - aus dem Doom-Port uebernommen
#endif
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    // Inputs ZUERST initialisieren, damit die USB-Modus-Taste sofort lesbar ist
    const int btnPins[] = {
        PB_PIN_JOY_UP, PB_PIN_JOY_DOWN, PB_PIN_JOY_LEFT, PB_PIN_JOY_RIGHT,
        PB_PIN_BTN_A, PB_PIN_BTN_B, PB_EXTRA_INPUT_PINS
    };
    for (int p : btnPins) {
        gpio_init(p);
        gpio_set_dir(p, false);
        gpio_pull_up(p);
    }

    // Display
    st7789_init();
    st7789_fill(0x0000);

    // Backlight-Pin von Digital-On auf PWM umstellen (st7789_init treibt
    // den Pin sonst hart auf HIGH). Spart auf Batterie merklich Strom.
    // Wrap=255 -> level entspricht direkt 8-bit Helligkeit.
    {
        gpio_set_function(PB_PIN_DISP_BL, GPIO_FUNC_PWM);
        uint slice = pwm_gpio_to_slice_num(PB_PIN_DISP_BL);
        uint chan  = pwm_gpio_to_channel(PB_PIN_DISP_BL);
        pwm_config cfg = pwm_get_default_config();
        pwm_config_set_wrap(&cfg, 255);
        pwm_init(slice, &cfg, true);
        pwm_set_chan_level(slice, chan, BACKLIGHT_LEVEL);
    }

    // Pull-up braucht ein paar Mikrosekunden zum Stabilisieren
    delay(5);

    // Palette schon HIER laden, damit Lade-Screens und Steuerungs-Splash
    // sie nutzen koennen. (gb_init_lcd kommt erst spaeter, ist aber egal -
    // manual_assign_palette + rebuild_fast_palette brauchen kein gb_state.)
    manual_palette = DEFAULT_PALETTE_INDEX;
    manual_assign_palette(palette, manual_palette);
    apply_palette_brightness_lift(BACKLIGHT_PALETTE_LIFT);
    rebuild_fast_palette();

    // ---- Modus-Verzweigung -----------------------------------------------
    // B beim Boot gehalten -> USB-Stick-Modus (kehrt nicht zurueck).
    if (!gpio_get(PB_PIN_USB_MODE)) {
        run_usb_stick_mode();
        // unreachable
    }

    // Game-Boy-Rahmen fuer Menue und Spiel
    draw_bezel();

    // ---- FatFS mounten + scannen (kein Startbild) -------------------------
    bool fs_ok = fs_mount();
    if (!fs_ok) {
        // First boot: the region has never been formatted -> make an empty FS.
        if (fs_format()) fs_ok = fs_mount();
    }
    if (fs_ok) {
        fatfs_ensure_welcome();
        fatfs_scan_gb_files();
    }

    // ---- Menue + ROM-Auswahl ---------------------------------------------
    if (fs_ok) {
        int idx = run_main_menu();
        // 4-KB-Puffer fuer ein eventuelles Umkopieren (Frame-Buffer, noch frei).
        fatfs_select_rom(g_rom_names[idx], fs_scratch_4k());
        fs_unmount();
    } else {
        // Format failed -> flash region unwritable. Halt with a hint.
        gb_screen_message("NO FILESYSTEM", "flash region", "unwritable?");
        while (true) tight_loop_contents();
    }

    // USB stick mode is NOT started here - we only emulate, never act as a
    // drive at the same time.

    // active_rom zeigt jetzt auf die Datei im Flash (fatfs_select_rom).
    // Bank 0 in SRAM cachen (entlastet XIP); kleine ROMs nicht ueberlesen.
    memset(rom_bank0_cache, 0xFF, sizeof(rom_bank0_cache));
    memcpy(rom_bank0_cache, active_rom,
           active_rom_len < sizeof(rom_bank0_cache) ? active_rom_len : sizeof(rom_bank0_cache));

    // Peanut-GB
    enum gb_init_error_e ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                                       &gb_cart_ram_write, &gb_error, NULL);
    if (ret != GB_INIT_NO_ERROR) {
        show_fatal_error("GB INIT FAILED", "ROM may be invalid.");
        // unreachable
    }

    // Feste Farbpalette wurde bereits frueh in setup() geladen.

#if ENABLE_LCD
    gb_init_lcd(&gb, &lcd_draw_line);
#endif

#if ENABLE_SOUND
    audio_init();
    // PWM auf dem Lautsprecher-Pin nur, wenn Ton eingeschaltet ist. Beim
    // alten PicoBoy Color brach mit laufendem PWM (50 % Tastverhaeltnis,
    // auch bei "Audio: OFF") die Versorgung zusammen: Neustart direkt nach
    // dem Spielstart.
    if (g_audio_enabled) {
        audio_pwm_set_bias((uint16_t)kAudioMid, (uint16_t)kAudioSwing);
        audio_pwm_init(AUDIO_SAMPLE_RATE);
    }
#endif

    // Save laden, falls vorhanden
    uint32_t save_size = gb_get_save_size(&gb);
    if (save_size > 0 && save_size <= sizeof(cart_ram)) {
        read_save(cart_ram, save_size);
    }

    // Steuerungs-Splash 2 Sekunden lang im GB-Bildschirm-Bereich,
    // mit der gleichen Palette die der Emulator nachher nutzt.
    {
        draw_bezel();
        st7789_fill_rect(GB_VIEW_X, GB_VIEW_Y, GB_VIEW_W, GB_VIEW_H, palette_color(0));
        // 2px Rahmen
        st7789_fill_rect(GB_VIEW_X,                 GB_VIEW_Y,                 GB_VIEW_W, 2, palette_color(3));
        st7789_fill_rect(GB_VIEW_X,                 GB_VIEW_Y + GB_VIEW_H - 2, GB_VIEW_W, 2, palette_color(3));
        st7789_fill_rect(GB_VIEW_X,                 GB_VIEW_Y,                 2, GB_VIEW_H, palette_color(3));
        st7789_fill_rect(GB_VIEW_X + GB_VIEW_W - 2, GB_VIEW_Y,                 2, GB_VIEW_H, palette_color(3));

        uint16_t bg = palette_color(0);
        uint16_t fg = palette_color(3);

        // Titel zentriert
        const char *title = "CONTROLS";
        int tx = GB_VIEW_X + (GB_VIEW_W - (int)strlen(title) * 8) / 2;
        st7789_draw_text(tx, GB_VIEW_Y + 12, title, fg, bg, 1);

        // 5 Zeilen Mapping, links eingerueckt
        int x = GB_VIEW_X + 12;
        int y = GB_VIEW_Y + 36;
        st7789_draw_text(x, y,      "DPAD  : " PB_TXT_DPAD,   fg, bg, 1); y += 14;
        st7789_draw_text(x, y,      "A     : " PB_TXT_A,      fg, bg, 1); y += 14;
        st7789_draw_text(x, y,      "B     : " PB_TXT_B,      fg, bg, 1); y += 14;
        st7789_draw_text(x, y,      "START : " PB_TXT_START,  fg, bg, 1); y += 14;
        st7789_draw_text(x, y,      "SELECT: " PB_TXT_SELECT, fg, bg, 1);

        delay(2000);
        // Display-Flaeche leeren; der Rahmen bleibt, das Spiel zeichnet hinein.
        st7789_fill_rect(kViewX, kViewY, kViewW, kViewH, palette_color(0));
    }

    // Display-Worker auf Core 1 starten, DMA-Channel reservieren.
    // Ab jetzt schreibt lcd_draw_line nur noch in den Frame-Buffer und
    // signalisiert Core 1 am Frame-Ende.
    start_display_core();
}

// ===========================================================================
// Main Loop
// ===========================================================================

void loop() {
    // ---- Ein Frame emulieren ----
    gb.gb_frame = 0;
    do {
        __gb_step_cpu(&gb);
    } while (!gb.gb_frame);

#if ENABLE_SOUND
    if (g_audio_enabled && !gb.direct.frame_skip) {
        audio_callback(NULL, audio_buffer, AUDIO_BUFFER_SIZE_BYTES);
        audio_pwm_push(audio_buffer, AUDIO_SAMPLES);
    }
#endif

    // ---- Input direkt auf GB-Joypad mappen (peanut_gb erwartet aktive-LOW) ----
    PbInput in = read_buttons();
    gb.direct.joypad_bits.up     = !in.dpad_up;
    gb.direct.joypad_bits.down   = !in.dpad_down;
    gb.direct.joypad_bits.left   = !in.dpad_left;
    gb.direct.joypad_bits.right  = !in.dpad_right;
    gb.direct.joypad_bits.a      = !in.btn_a;
    gb.direct.joypad_bits.b      = !in.btn_b;
    gb.direct.joypad_bits.start  = !in.btn_start;
    gb.direct.joypad_bits.select = !in.btn_select;

    // ---- Bremse auf Originaltempo (59,73 Bilder/s = 16 742 us) ----
    // Ohne sie laeuft der Emulator bei ruhigem Bild (Dirty-Box leer, Ton aus)
    // schneller als ein echter Game Boy: Titelbilder und Demos rasen durch.
    // Ist der Emulator zu langsam, wird nicht aufgeholt, sondern neu angesetzt.
    static uint64_t next_frame_us = 0;
    const uint64_t kFrameUs = 16742;
    uint64_t now = time_us_64();
    if (next_frame_us == 0 || now > next_frame_us + kFrameUs) {
        next_frame_us = now + kFrameUs;
    } else {
        while (time_us_64() < next_frame_us) tight_loop_contents();
        next_frame_us += kFrameUs;
    }
}

// ===========================================================================
// Entry point
// ---------------------------------------------------------------------------
// The Arduino core provided the setup()/loop() runtime; under the plain SDK
// we drive it ourselves: setup() once, then loop() forever. (setup() itself
// either enters USB stick mode and never returns, or falls through into the
// emulator once a ROM has been selected.)
// ===========================================================================
int main() {
    setup();
    for (;;) {
        loop();
    }
}
