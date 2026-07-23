/*
 * ibuttonIO - iButton reader / cloner with 1602A LCD
 * ------------------------------------------------------------
 * Board : Arduino Uno / Nano (AVR)
 * Library: iButtonTag  (vdwulp/iButtonTag)  https://github.com/vdwulp/iButtonTag
 *          LiquidCrystal (built-in)
 *
 * FUNCTION
 *   The device holds NUM_SLOTS independent memory slots. The button selects
 *   which slot is active; the slot's contents decide what the slot does:
 *
 *     empty slot  -> READ  : reads any iButton touched to the probe and
 *                            stores its code into the active slot.
 *     filled slot -> WRITE : auto-clones the slot's code onto any WRITABLE
 *                            iButton (RW1990 / RW2004 / TM01 ...) touched.
 *
 *   Button   : short press  -> select the next slot (wraps around);
 *              long press (1s) -> clear the active slot (RAM + EEPROM),
 *                                 which turns it back into a READ slot.
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
 *   11  D4               -> D6
 *   12  D5               -> D5
 *   13  D6               -> D4
 *   14  D7               -> D3
 *   15  A  (backlight +) -> 5V through a 220 ohm resistor
 *   16  K  (backlight -) -> GND
 *
 *   iButton probe: DATA -> D10, with a 4.7k ohm pull-up resisor to 5V
 *                  GND  -> GND
 *
 *   Slot button: between D2 and GND (uses internal pull-up).
 *                D2 is INT0, the Nano/Uno hardware interrupt pin, so the
 *                press is caught instantly even while the LCD delays run.
 *
 *   Built-in LED (D13) is used as a read/write activity blink.
 * ------------------------------------------------------------
 */

#include <iButtonTag.h>
#include <LiquidCrystal.h>
#include <EEPROM.h>

// ---- Config ----
#define NUM_SLOTS 5      // number of memory slots the button cycles through

// ---- Pins ----
#define IBUTTON_PIN 10   // = digital pin 10 on Uno/Nano
#define BUTTON_PIN  2    // slot select button -> GND (D2 = INT0 hardware interrupt)
LiquidCrystal lcd(12, 11, 6, 5, 4, 3);

iButtonTag ibutton(IBUTTON_PIN);

// ---- EEPROM layout ----
// Each slot is stored as [magic][8 code bytes]. A slot whose magic byte is not
// EE_SLOT_MAGIC is considered empty. Slot i starts at i * EE_SLOT_SIZE.
#define EE_SLOT_MAGIC 0xB7
#define EE_SLOT_SIZE  9    // 1 magic byte + 8 code bytes

// ---- State ----
iButtonCode savedCode[NUM_SLOTS];        // code stored in each slot (uint8_t[8])
bool  haveSaved[NUM_SLOTS];              // is the slot's code valid?
uint8_t curSlot     = 0;                 // currently selected slot (0..NUM_SLOTS-1)
bool  tagPresent    = false;             // READ: a tag is currently on the probe
bool  writeWait     = false;             // WRITE: waiting for tag removal after a write

// A slot with a stored code is a WRITE slot; an empty slot is a READ slot.
inline bool writeMode() { return haveSaved[curSlot]; }

// ---- Button debounce / long-press ----
// The button sits on INT0 (D2). The ISR fires on every edge and records the
// press/release into these volatile fields; loop() reads them to decide short
// vs long press. Capturing the edge in the ISR means a tap is never lost while
// doRead()/doWrite() are blocked inside their delay() calls.
volatile bool          btnDown       = false; // button currently held (debounced)
volatile unsigned long btnDownTime   = 0;     // millis() when the press started
volatile bool          btnShortPend  = false; // ISR flagged a short-press release
volatile bool          longFired     = false; // long-press already handled for this hold
volatile unsigned long btnLastEdge   = 0;     // millis() of last accepted edge (bounce filter)
const unsigned long BTN_DEBOUNCE_MS = 30;
const unsigned long BTN_LONG_MS     = 500;  // hold this long to clear the slot

// Button events
#define EV_NONE  0
#define EV_SHORT 1   // short press -> select next slot
#define EV_LONG  2   // long press  -> clear active slot

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

// Draw line 0 = slot + mode + status, line 1 = active slot's code (or a placeholder).
void drawScreen(const char *status) {
  char line[17];

  // Line 0: "<slot> <mode>" on the left, status right-aligned.
  memset(line, ' ', 16);
  line[16] = '\0';

  uint8_t slen = (status && *status) ? strlen(status) : 0;
  if (slen > 16) slen = 16;

  // Left label e.g. "1/5 WRITE"
  char label[13];
  snprintf(label, sizeof(label), "%u/%u %s",
           (unsigned)(curSlot + 1), (unsigned)NUM_SLOTS,
           writeMode() ? "WRITE" : "READ");
  uint8_t llen = strlen(label);
  if (llen > 16) llen = 16;

  // Show the label only if it doesn't collide with the status (needs at least a
  // 1-char gap). Otherwise the status gets the whole row.
  if (llen + 1 + slen <= 16) memcpy(line, label, llen);
  if (slen) memcpy(line + (16 - slen), status, slen);   // right-aligned
  lcd.setCursor(0, 0);
  lcd.print(line);

  // Line 1: the active slot's code
  lcd.setCursor(0, 1);
  if (haveSaved[curSlot]) {
    codeToHex(savedCode[curSlot], line);
    lcd.print(line);
  } else {
    lcd.print("  -- no code -- ");
  }
}

// Flash the active slot's code line a few times to indicate a read/write happened.
void flashCode() {
  if (!haveSaved[curSlot]) return;
  char hex[17];
  codeToHex(savedCode[curSlot], hex);
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

uint16_t slotAddr(uint8_t slot) { return (uint16_t)slot * EE_SLOT_SIZE; }

void saveSlotToEEPROM(uint8_t slot) {
  uint16_t addr = slotAddr(slot);
  EEPROM.update(addr, EE_SLOT_MAGIC);
  for (uint8_t i = 0; i < 8; i++) EEPROM.update(addr + 1 + i, savedCode[slot][i]);
}

bool loadSlotFromEEPROM(uint8_t slot) {
  uint16_t addr = slotAddr(slot);
  if (EEPROM.read(addr) != EE_SLOT_MAGIC) return false;
  for (uint8_t i = 0; i < 8; i++) savedCode[slot][i] = EEPROM.read(addr + 1 + i);
  return ibutton.testCode(savedCode[slot]) == 1;   // 1 = structurally valid
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------
void setup() {
  Serial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, CHANGE);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  lcd.begin(16, 2);
  lcd.print("  ibutton I/O");
  lcd.setCursor(0, 1);
  lcd.print("  reader/cloner");
  delay(1000);

  for (uint8_t s = 0; s < NUM_SLOTS; s++) haveSaved[s] = loadSlotFromEEPROM(s);
  curSlot = 0;

  lcd.clear();
  drawScreen("ready");
}

// ------------------------------------------------------------
// Button ISR (INT0 on D2, triggered on CHANGE).
//   Captures each edge the instant it happens. A crude time-window filter
//   swallows contact bounce: edges arriving within BTN_DEBOUNCE_MS of the last
//   accepted edge are ignored (a human tap is far longer than that, so no real
//   press is lost). Note: millis() does not advance inside an ISR, but its last
//   value is a fine timestamp for "now".
// ------------------------------------------------------------
void buttonISR() {
  unsigned long now = millis();
  if (now - btnLastEdge < BTN_DEBOUNCE_MS) return;   // ignore contact bounce
  btnLastEdge = now;

  if (digitalRead(BUTTON_PIN) == LOW) {              // pressed (pull-up -> LOW)
    btnDown     = true;
    btnDownTime = now;
    longFired   = false;
  } else {                                           // released
    if (btnDown && !longFired) btnShortPend = true;  // short press -> handle in loop
    btnDown = false;
  }
}

// ------------------------------------------------------------
// Button: turn the ISR-captured edges into short/long events.
//   - Long press  (held past BTN_LONG_MS) -> EV_LONG once, while still held.
//   - Short press (released before that)  -> EV_SHORT on release.
// Reads of the multi-byte volatile are guarded so the ISR can't tear them.
// ------------------------------------------------------------
uint8_t buttonEvent() {
  noInterrupts();
  bool          down     = btnDown;
  bool          lf       = longFired;
  unsigned long downTime = btnDownTime;
  bool          shortEv  = btnShortPend;
  interrupts();

  // Ground-truth check against the pin. The ISR's time-window debounce can
  // swallow the release edge of a quick tap (if it lands within
  // BTN_DEBOUNCE_MS of the press), leaving btnDown stuck true. Left alone,
  // the timer below would then mistake that tap for a 1s hold and wrongly
  // clear the slot. If we think the button is held but the pin has actually
  // gone high, the release was missed: drop the stuck state and treat it as
  // the short press it was.
  if (down && digitalRead(BUTTON_PIN) == HIGH) {
    noInterrupts();
    btnDown = false;
    interrupts();
    down = false;
    if (!lf && !shortEv) shortEv = true;   // recover the missed short-press
  }

  // Fire the long-press once, while the button is still held.
  if (down && !lf && millis() - downTime > BTN_LONG_MS) {
    noInterrupts();
    longFired    = true;
    btnShortPend = false;   // the eventual release must not also count as short
    interrupts();
    return EV_LONG;
  }

  if (shortEv) {
    noInterrupts();
    btnShortPend = false;
    interrupts();
    return EV_SHORT;
  }
  return EV_NONE;
}

// Select the next slot (wraps around) and reset the per-slot activity state.
void nextSlot() {
  curSlot = (curSlot + 1) % NUM_SLOTS;
  tagPresent = false;
  writeWait  = false;
  drawScreen("ready");
}

// Wipe the active slot's code from RAM and invalidate it in EEPROM.
// This turns the slot back into a READ slot.
void clearSlot() {
  EEPROM.update(slotAddr(curSlot), 0xFF);   // invalidate the marker
  memset(savedCode[curSlot], 0, 8);
  haveSaved[curSlot] = false;
  tagPresent = false;
  writeWait  = false;
  Serial.print("Slot "); Serial.print(curSlot + 1); Serial.println(" cleared");

  drawScreen("cleared");
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
  delay(600);
  drawScreen("ready");
}

// ------------------------------------------------------------
// READ mode  (active slot is empty -> capture a code into it)
// ------------------------------------------------------------
void doRead() {
  iButtonCode code;
  int8_t status = ibutton.readCode(code);

  if (status == 1) {                 // valid code on the probe
    if (!tagPresent) {               // new contact -> capture it once
      tagPresent = true;
      memcpy(savedCode[curSlot], code, 8);
      haveSaved[curSlot] = true;     // slot now filled -> becomes a WRITE slot
      saveSlotToEEPROM(curSlot);

      Serial.print("Read into slot "); Serial.print(curSlot + 1); Serial.print(": ");
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
// WRITE mode  (active slot filled -> auto-clone its code onto a writable tag)
// ------------------------------------------------------------
void doWrite() {
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

  int8_t r = ibutton.writeCode(savedCode[curSlot]);   // auto-detect writable type

  if (r == 1) {                    // success
    Serial.print("Write OK from slot "); Serial.println(curSlot + 1);
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
    clearSlot();
  } else if (ev == EV_SHORT) {
    nextSlot();
  }

  if (writeMode()) doWrite();
  else             doRead();
}
