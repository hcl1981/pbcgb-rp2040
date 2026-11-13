/*
 * picoboy_pins.h
 *
 * Hardware-Pin-Mapping. Zwei Geraete, gewaehlt ueber PB_BOARD_PICOBOY
 * (CMakeLists.txt baut fuer jedes eine eigene Firmware):
 *
 *   PB_BOARD_PICOBOY 0  RP2040 mit 2-Zoll-Waveshare-Display 19888
 *                       (ST7789, 240x320, Hochformat) und Einzeltasten,
 *                       ohne Lautsprecher            -> picoboygb.uf2
 *   PB_BOARD_PICOBOY 1  alter PicoBoy Color: RP2040, 2 MB Flash, Pins wie
 *                       beim PicoBoy Color Plus (ST7789 240x280, 5-Wege-
 *                       Joystick, A/B, Piezo)        -> picoboygb_pbc.uf2
 *
 * Alle Tasten sind aktiv-LOW mit internem Pull-up.
 */

#ifndef PICOBOY_PINS_H
#define PICOBOY_PINS_H

#ifndef PB_BOARD_PICOBOY
#define PB_BOARD_PICOBOY 0
#endif

#if PB_BOARD_PICOBOY
// ===========================================================================
// Alter PicoBoy Color
// ===========================================================================

// ---- Display (ST7789, 240x280, SPI0) ----
#define PB_PIN_DISP_SCK   18    // SPI0 SCK
#define PB_PIN_DISP_MOSI  19    // SPI0 TX
#define PB_PIN_DISP_CS    10
#define PB_PIN_DISP_DC     8
#define PB_PIN_DISP_RST    9
#define PB_PIN_DISP_BL    26    // PWM-faehig

#define PB_DISP_W         240
#define PB_DISP_H         280
#define PB_DISP_Y_OFFSET   20   // Panel sitzt mit 20px Offset im 240x320-RAM
#define PB_DISP_X_OFFSET    0
#define PB_DISP_MADCTL    0xC8  // 180 Grad, BGR

// ---- Eingaben (5-Wege-Joystick + 2 Action-Buttons) ----
#define PB_PIN_JOY_UP      4
#define PB_PIN_JOY_DOWN    2
#define PB_PIN_JOY_LEFT    3
#define PB_PIN_JOY_RIGHT   1
#define PB_PIN_JOY_CENTER  0
#define PB_PIN_BTN_A      27
#define PB_PIN_BTN_B      28

#define PB_HAS_CENTER      1    // START/SELECT = Mitte + B / Mitte + A
#define PB_PIN_USB_MODE    PB_PIN_JOY_CENTER   // beim RESET halten
#define PB_PIN_MENU_OK     PB_PIN_JOY_CENTER   // bestaetigt im Menue (neben A/B)
#define PB_EXTRA_INPUT_PINS PB_PIN_JOY_CENTER

// ---- Audio (Piezo via PWM mit RC-Tiefpass, GP15 = AUDIO_PWM_PIN) ----
#define PB_HAS_SPEAKER     1
#define PB_PIN_SPEAKER    15
// Lautsprecher haengt an einem Transistor: bei 50 % Ruhepegel brach die
// Versorgung zusammen. Ruhepunkt 12,5 %, Ausschlag +-12,5 %, sanfter Start.
#define AUDIO_PWM_MID      128
#define AUDIO_PWM_SWING    128
#define AUDIO_PWM_RAMP_MS  200

// GB-A liegt auf der Taste B, GB-B auf der Taste A (wie beim PBC+-GBC-
// Emulator). START/SELECT bleiben Mitte + B / Mitte + A.
#define PB_SWAP_AB         1

// ---- Texte auf dem Display ----
#define PB_TXT_USB_KEY    "CENTER"
#define PB_TXT_OK_KEY     "CENTER"
#define PB_TXT_DPAD       "JOY"
#define PB_TXT_A          "BTN B"
#define PB_TXT_B          "BTN A"
#define PB_TXT_START      "CTR + B"
#define PB_TXT_SELECT     "CTR + A"

#else
// ===========================================================================
// RP2040 + Waveshare-Display 19888 + Einzeltasten
// ===========================================================================

// ---- Display (ST7789, 240x320, SPI0) ----
#define PB_PIN_DISP_SCK   18    // SPI0 SCK
#define PB_PIN_DISP_MOSI  19    // SPI0 TX
#define PB_PIN_DISP_CS    21
#define PB_PIN_DISP_DC    17
#define PB_PIN_DISP_RST   20
#define PB_PIN_DISP_BL    16    // PWM-faehig

#define PB_DISP_W         240
#define PB_DISP_H         320
#define PB_DISP_Y_OFFSET    0   // rowstart=0
#define PB_DISP_X_OFFSET    0   // colstart=0
// Wie adafruit_st7789 unter CircuitPython (rotation=0): MY|MX, RGB.
// Steht das Bild auf dem Kopf: 0x00. Sind Rot und Blau vertauscht: 0xC8.
#define PB_DISP_MADCTL    0xC0

// ---- Eingaben: linke Reihe (Steuerkreuz + A/B) ----
#define PB_PIN_JOY_UP      8
#define PB_PIN_JOY_DOWN    7
#define PB_PIN_JOY_LEFT    9
#define PB_PIN_JOY_RIGHT   6
#define PB_PIN_BTN_A       4
#define PB_PIN_BTN_B       5

// ---- Eingaben: externe Tasten (GP0, GP1, GP2, GP3, GP27, GP28) ----
#define PB_PIN_BTN_SELECT  0
#define PB_PIN_BTN_START   1

#define PB_HAS_CENTER      0    // START/SELECT = Links+Rechts / Hoch+Runter
#define PB_PIN_USB_MODE    PB_PIN_BTN_B        // beim RESET halten
#define PB_PIN_MENU_OK     PB_PIN_BTN_START    // bestaetigt im Menue (neben A/B)
#define PB_EXTRA_INPUT_PINS PB_PIN_BTN_START, PB_PIN_BTN_SELECT

// ---- Kein Lautsprecher ----
#define PB_HAS_SPEAKER     0

// ---- Texte auf dem Display ----
#define PB_TXT_USB_KEY    "B"
#define PB_TXT_OK_KEY     "START"
#define PB_TXT_DPAD       "GP6-9"
#define PB_TXT_A          "GP4"
#define PB_TXT_B          "GP5"
#define PB_TXT_START      "LEFT+RIGHT"
#define PB_TXT_SELECT     "UP+DOWN"

#endif

#endif // PICOBOY_PINS_H
