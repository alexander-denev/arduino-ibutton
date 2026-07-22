/*
 * ibuttonIO - iButton reader / cloner with 1602A LCD
 * ------------------------------------------------------------
 * Board : Arduino Uno / Nano (AVR)
 * Library: iButtonTag  (vdwulp/iButtonTag)  https://github.com/vdwulp/iButtonTag
 *          LiquidCrystal (built-in)
 *
 * FUNCTION
 *   READ mode : continuously reads any iButton touched to the probe,
 *               saves the code (RAM + EEPROM) and flashes it on the LCD.
 *   WRITE mode: auto-clones the saved code onto any WRITABLE iButton
 *               (RW1990 / RW2004 / TM01 ...) the moment it touches the probe.
 *   Button   : short press toggles READ/WRITE; long press (1s) clears
 *               the saved code from RAM and EEPROM.
 *
 * ------------------------------------------------------------
 * WIRING (bare 16-pin 1602A, 4-bit parallel)
 *
 *   LCD pin              -> Arduino
 *    1  VSS  (GND)       -> GND
 *    2  VDD  (+5V)       -> 5V
 *    3  V0   (contrast)  -> wiper of a 10k pot (pot ends -> 5V and GND)
 *    4  RS               -> D12
 *    5  RW               -> GND        (write-only, tie low)
 *    6  E                -> D11
 *    7-10 D0-D3          -> not connected (4-bit mode)
 *   11  D4               -> D5
 *   12  D5               -> D4
 *   13  D6               -> D3
 *   14  D7               -> D2
 *   15  A  (backlight +) -> 5V through a 220 ohm resistor
 *   16  K  (backlight -) -> GND
 *
 *   iButton probe: DATA -> D10, with a 4.7k ohm pull-up resisor to 5V
 *                  GND  -> GND
 *
 *   Mode button: between D8 and GND (uses internal pull-up)
 *
 *   Built-in LED (D13) is used as a read/write activity blink.
 * ------------------------------------------------------------
 */

#include <iButtonTag.h>
#include <LiquidCrystal.h>
#include <EEPROM.h>

// ---- Pins ----
#define IBUTTON_PIN 10   // = digital pin 10 on Uno/Nano
#define BUTTON_PIN  8    // mode toggle button -> GND
// LCD: RS, E, D4, D5, D6, D7
LiquidCrystal lcd(12, 11, 5, 4, 3, 2);

iButtonTag ibutton(IBUTTON_PIN);

// ---- EEPROM layout ----
#define EE_MAGIC_ADDR 0
#define EE_CODE_ADDR  1
#define EE_MAGIC      0xB7   // marker: a valid saved code lives in EEPROM

// ---- State ----
iButtonCode savedCode;          // last read / stored code (uint8_t[8])
bool  haveSaved     = false;    // is savedCode valid?
bool  writeMode     = false;    // false = READ, true = WRITE
bool  tagPresent    = false;    // READ: a tag is currently on the probe
bool  writeWait     = false;    // WRITE: waiting for tag removal after a write

// ---- Button debounce / long-press ----
int   btnStable     = HIGH;     // debounced level (HIGH = released, pull-up)
int   btnLastRead   = HIGH;
unsigned long btnLastChange = 0;
bool  btnDown       = false;    // button currently held (debounced)
unsigned long btnDownTime = 0;  // when the current press started
bool  longFired     = false;    // long-press already handled for this hold
const unsigned long BTN_DEBOUNCE_MS = 40;
const unsigned long BTN_LONG_MS     = 1000;  // hold this long to clear memory

// Button events
#define EV_NONE  0
#define EV_SHORT 1   // short press -> toggle mode
#define EV_LONG  2   // long press  -> clear saved code

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

// Convert the 8-byte code to 16 uppercase hex chars (fills the LCD row).
void codeToHex(iButtonCode code, char *buf) {
  const char *h = "0123456789ABCDEF";
  for (uint8_t i = 0; i < 8; i++) {
    buf[i * 2]     = h[(code[i] >> 4) & 0x0F];
    buf[i * 2 + 1] = h[code[i] & 0x0F];
  }
  buf[16] = '\0';
}

// Draw line 0 = mode + status, line 1 = saved code (or a placeholder).
void drawScreen(const char *status) {
  char line[17];

  // Line 0: mode on the left, status right-aligned.
  memset(line, ' ', 16);
  line[16] = '\0';

  uint8_t slen = (status && *status) ? strlen(status) : 0;
  if (slen > 16) slen = 16;

  const char *mode = writeMode ? "WRITE" : "READ";
  uint8_t mlen = strlen(mode);

  // Show the mode label only if it doesn't collide with the status
  // (needs at least a 1-char gap). Otherwise the status gets the whole row.
  if (mlen + 1 + slen <= 16) memcpy(line, mode, mlen);
  if (slen) memcpy(line + (16 - slen), status, slen);   // right-aligned
  lcd.setCursor(0, 0);
  lcd.print(line);

  // Line 1: the saved code
  lcd.setCursor(0, 1);
  if (haveSaved) {
    codeToHex(savedCode, line);
    lcd.print(line);
  } else {
    lcd.print("  -- no code -- ");
  }
}

// Flash the code line a few times to indicate a read/write happened.
void flashCode() {
  if (!haveSaved) return;
  char hex[17];
  codeToHex(savedCode, hex);
  for (uint8_t i = 0; i < 3; i++) {
    lcd.setCursor(0, 1);
    lcd.print("                ");   // blank the row
    digitalWrite(LED_BUILTIN, HIGH);
    delay(120);
    lcd.setCursor(0, 1);
    lcd.print(hex);
    digitalWrite(LED_BUILTIN, LOW);
    delay(160);
  }
}

void saveToEEPROM() {
  EEPROM.update(EE_MAGIC_ADDR, EE_MAGIC);
  for (uint8_t i = 0; i < 8; i++) EEPROM.update(EE_CODE_ADDR + i, savedCode[i]);
}

bool loadFromEEPROM() {
  if (EEPROM.read(EE_MAGIC_ADDR) != EE_MAGIC) return false;
  for (uint8_t i = 0; i < 8; i++) savedCode[i] = EEPROM.read(EE_CODE_ADDR + i);
  return ibutton.testCode(savedCode) == 1;   // 1 = structurally valid
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------
void setup() {
  Serial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  lcd.begin(16, 2);
  lcd.print("  ibutton I/O");
  lcd.setCursor(0, 1);
  lcd.print("  reader/cloner");
  delay(1000);

  haveSaved = loadFromEEPROM();
  lcd.clear();
  drawScreen(haveSaved ? "ready" : "no code");
}

// ------------------------------------------------------------
// Button: debounced short/long press detection.
//   - Short press (released before BTN_LONG_MS) -> EV_SHORT on release.
//   - Long press  (held past BTN_LONG_MS)       -> EV_LONG once while held.
// ------------------------------------------------------------
uint8_t buttonEvent() {
  int reading = digitalRead(BUTTON_PIN);
  if (reading != btnLastRead) {
    btnLastChange = millis();
    btnLastRead = reading;
  }

  if (millis() - btnLastChange > BTN_DEBOUNCE_MS && reading != btnStable) {
    btnStable = reading;
    if (btnStable == LOW) {              // pressed
      btnDown = true;
      btnDownTime = millis();
      longFired = false;
    } else {                            // released
      bool wasShort = btnDown && !longFired;
      btnDown = false;
      if (wasShort) return EV_SHORT;
    }
  }

  // Fire the long-press once, while the button is still held.
  if (btnDown && !longFired && millis() - btnDownTime > BTN_LONG_MS) {
    longFired = true;
    return EV_LONG;
  }
  return EV_NONE;
}

// Wipe the saved code from RAM and invalidate it in EEPROM.
void clearMemory() {
  EEPROM.update(EE_MAGIC_ADDR, 0xFF);   // invalidate the marker
  memset(savedCode, 0, 8);
  haveSaved  = false;
  tagPresent = false;
  writeWait  = false;
  Serial.println("Memory cleared");

  drawScreen("cleared");
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
  delay(600);
  drawScreen("no code");
}

// ------------------------------------------------------------
// READ mode
// ------------------------------------------------------------
void doRead() {
  iButtonCode code;
  int8_t status = ibutton.readCode(code);

  if (status == 1) {                 // valid code on the probe
    if (!tagPresent) {               // new contact -> capture it once
      tagPresent = true;
      memcpy(savedCode, code, 8);
      haveSaved = true;
      saveToEEPROM();

      Serial.print("Read: ");
      ibutton.printCode(code);
      Serial.println();

      drawScreen("saved");
      flashCode();
      drawScreen("ready");
    }
  } else if (status == 0) {          // nothing on the probe
    tagPresent = false;
  } else if (!tagPresent) {          // -1 bad checksum / -2 all zero
    tagPresent = true;               // a tag responded but is unreadable
    drawScreen(status == -1 ? "bad crc" : "empty");
    delay(600);
    drawScreen("ready");
  }
  delay(120);
}

// ------------------------------------------------------------
// WRITE mode  (auto-clone saved code onto a writable tag)
// ------------------------------------------------------------
void doWrite() {
  if (!haveSaved) {
    drawScreen("no code");
    delay(200);
    return;
  }

  // After a write, wait until the tag is lifted before writing again.
  if (writeWait) {
    iButtonCode tmp;
    if (ibutton.readCode(tmp) == 0) {
      writeWait = false;
      drawScreen("ready");
    }
    delay(100);
    return;
  }

  int8_t r = ibutton.writeCode(savedCode);   // auto-detect writable type

  if (r == 1) {                    // success
    Serial.println("Write OK");
    drawScreen("OK!");
    flashCode();
    writeWait = true;
  } else if (r == 0) {             // no tag present yet -> keep waiting quietly
    // nothing
  } else {                         // some failure
    const char *msg;
    if (r >= -9)        msg = "code err";   // -1..-9  saved code problem
    else if (r >= -19)  msg = "read-only";  // -11..-19 type problem
    else                msg = "write fail"; // -21..-29 write problem
    Serial.print("Write err "); Serial.println(r);
    drawScreen(msg);
    delay(1000);
    drawScreen("ready");
    writeWait = true;             // require removal before retrying
  }
  delay(60);
}

// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------
void loop() {
  uint8_t ev = buttonEvent();
  if (ev == EV_LONG) {
    clearMemory();
  } else if (ev == EV_SHORT) {
    writeMode  = !writeMode;
    tagPresent = false;
    writeWait  = false;
    drawScreen("ready");
  }

  if (writeMode) doWrite();
  else           doRead();
}
