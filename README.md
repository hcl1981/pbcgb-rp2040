# pbcgb-rp2040

Game-Boy-Emulator (DMG) für den **RP2040** mit 2 MB Flash, auf Grundlage von
[Peanut-GB](https://github.com/deltabeard/Peanut-GB). Ein Quelltext, zwei
Firmwares:

| Datei | Gerät |
|---|---|
| `build/picoboygb.uf2` | Raspberry Pi Pico mit **Waveshare 2-Zoll-Display 19888** (ST7789, 240×320, Hochformat) und Einzeltastern, ohne Lautsprecher |
| `build/picoboygb_pbc.uf2` | **alter PicoBoy Color** (RP2040, 2 MB Flash, ST7789 240×280, 5-Wege-Joystick, A/B, Lautsprecher an GP15) |

Die Pins stehen in [`src/picoboy_pins.h`](src/picoboy_pins.h), umgeschaltet
über `PB_BOARD_PICOBOY`.

* Bild 1,25-fach (200×180) in einem Rahmen im Stil des ersten Game Boy: graue
  Scheibe über den ganzen Bildschirm, zwei Streifen mit „DOT MATRIX“
* 200 MHz bei 1,15 V, volle 59,7 Bilder/s, Tempo auf das Original begrenzt
* Kern 0 emuliert, Kern 1 schickt per DMA nur die geänderten Bildbereiche
  ans Display
* ROMs liegen auf einem USB-Laufwerk im Flash (1,69 MB) und werden direkt
  von dort gelesen, ohne Umkopieren

## Download

Fertige Firmwares gibt es unter
[Releases](https://github.com/hcl1981/pbcgb-rp2040/releases).

## Bedienung

| | Waveshare-Gerät | PicoBoy Color |
|---|---|---|
| Steuerkreuz | GP8 hoch, GP7 runter, GP9 links, GP6 rechts | Joystick |
| Game-Boy-A | GP4 | Taste **B** |
| Game-Boy-B | GP5 | Taste **A** |
| START | Links + Rechts gleichzeitig (oder Taster an GP1) | Mitte + B |
| SELECT | Hoch + Runter gleichzeitig (oder Taster an GP0) | Mitte + A |
| USB-Stick-Modus | B halten + RESET | Mitte halten + RESET |
| Menü: wählen / starten | Hoch/Runter, A oder START | Hoch/Runter, A, B oder Mitte |
| Ton | – | Menüzeile „Audio“, Links/Rechts: OFF/ON |

1. Firmware flashen: BOOTSEL halten, USB anstecken, die passende `.uf2` auf
   das Laufwerk `RPI-RP2` kopieren.
2. **Spiele aufspielen:** USB-Stick-Modus (Tabelle). Das Gerät meldet sich
   als Laufwerk `PICOBOYGB`; `.gb`-Dateien draufkopieren und **auswerfen**.
   Beim ersten Mal wird das Laufwerk automatisch formatiert.
3. **RESET** ohne Taste: Menü mit Info, (Audio) und den Spielen.

Liegt eine Spieldatei nach Löschen und Neukopieren zerstückelt im Flash, wird
sie beim Start einmal zusammenhängend umgeschrieben („DEFRAGMENTING“). Reicht
der Platz dafür nicht („NOT ENOUGH ROOM“): Dateien löschen, Spiel neu kopieren.

## Bauen

```bash
./build.sh            # baut beide: build/picoboygb.uf2, build/picoboygb_pbc.uf2
./build.sh clean      # Bauordner löschen und neu bauen
```

Gebraucht werden das Pico SDK (getestet mit 2.2.0; `PICO_SDK_PATH`, sonst
`~/pico/pico-sdk`), `gcc-arm-none-eabi` und `cmake`.

| Option (`./build.sh -D…`) | Vorgabe | Bedeutung |
|---|---|---|
| `CPU_KHZ` | 200000 | Systemtakt in kHz |
| `CPU_VREG` | `VREG_VOLTAGE_1_15` | Kernspannung |
| `FLASH_CLKDIV` | 2 | Flash-Teiler Waveshare-Gerät (100 MHz) |
| `FLASH_CLKDIV_PBC` | 4 | Flash-Teiler PicoBoy Color (50 MHz, eigener Bootloader `bs2_pbc`) |
| `LCD_SPI_HZ` | 62500000 | angefragter Display-Takt; wird 200 MHz / 4 = 50 MHz |

Der Bau bricht ab, wenn eine Firmware größer als 256 KB wird (sie ragte sonst
ins Laufwerk); zurzeit sind es rund 85 KB.

## Ton am PicoBoy Color

Der Lautsprecher hängt an GP15 hinter einem Tiefpass, und die Versorgung des
alten PicoBoy Color verträgt keinen großen Dauerstrom. Am Gerät gemessen
(während der Entwicklung über ein USB-Protokoll):

| PWM-Ruhepegel | Ergebnis |
|---|---|
| 50 % (wie beim PicoBoy Color Plus) | Gerät geht sofort aus |
| 31 %, Ausschlag 512 | geht nach gut einer Minute aus |
| 31 %, Ausschlag 320 | **Einstellung „Audio: ON“** |
| 12,5 % | still |

Deshalb ist der Ton vorgegeben aus, und der PWM-Pin wird nur bei „Audio: ON“
überhaupt eingeschaltet (vorher lief er immer – das war der Absturz beim
Spielstart). 1-Bit- und Puls-Ausgabe wurden ausprobiert und klangen nicht
besser. Rechtecktöne in Notenfrequenz klingen dagegen sauber; das prüft der
Tontest unter [`tools/audiotest`](tools/audiotest).

Die Tonwerte entstehen mit 22 222 Hz, passend zum 45-µs-Takt der Ausgabe
(mit 22 050 Hz lief der Puffer leer und es knackte).

## Absturzanzeige

Ein Prozessorfehler bleibt als `CRASH core … PC … LR …` auf dem Display
stehen; `arm-none-eabi-addr2line -e build/<firmware>.elf <PC>` zeigt die
Stelle.

## Flash-Aufteilung (2 MB)

| Adresse | Größe | Inhalt |
|---|---|---|
| `0x000000` | 256 KB | Firmware |
| `0x040000` | 1,69 MB | FatFs-Laufwerk `PICOBOYGB` ([`src/flash_fs.h`](src/flash_fs.h)) |
| `0x1F0000` | 64 KB | Spielstand |

Ein FatFs-Sektor ist genau ein 4-KB-Flash-Sektor. Liegt eine Datei am Stück,
steht das ROM also linear im Flash und wird per XIP gelesen; Bank 0 (16 KB)
liegt zusätzlich im RAM.

## Bekannte Grenzen

* **Spielstände werden nicht geschrieben**, nur ein vorhandener geladen
  (`write_save()` wird nirgends aufgerufen).
* Nur der klassische Game Boy (DMG), kein Game Boy Color.
* Der PicoBoy-Color-Flash läuft vorsichtshalber mit 50 MHz; ob 100 MHz dort
  auch gehen, ist nicht geprüft.

## Behobene Fehler gegenüber der Vorlage

* Kern 1 las den geänderten Bildbereich als `int8_t`; bei 200×180 wurden
  Koordinaten ab 128 negativ, das Display blieb fast schwarz und zeigte nur
  Bruchstücke.
* Keine Tempobremse: Bei ruhigem Bild lief der Emulator schneller als das
  Original.
* PWM am Lautsprecher lief auch bei ausgeschaltetem Ton (siehe oben).

## Herkunft und Lizenz

MIT, wie alle Glieder der Kette (Peanut-GB → RP2040-GB → Pico-GB →
PicoBoyGB); Einzelheiten in [`HERKUNFT.md`](HERKUNFT.md) und [`LICENSE`](LICENSE).
`fatfs/`: ChaN FatFs mit eigener BSD-artiger Lizenz (im Dateikopf).

Es sind keine Spiele enthalten; ROMs bitte nur von eigenen Modulen verwenden.
