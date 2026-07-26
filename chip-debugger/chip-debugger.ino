/*
 * 1-Wire Passive Command Sniffer
 * ------------------------------------------------------------
 * Purpose: tap the DATA line between a REAL reader and an iButton
 *          (original or RW1990 clone) and print what the reader
 *          sends + the raw pulse timings, so you can see whether
 *          the reader does an anti-clone probe (0xD1/0xD5) or is
 *          rejecting the clone on timing.
 *
 * Pin    : D8 (PB0 on Uno/Nano), INPUT_PULLUP (passive tap, noise-guarded)
 * Baud   : 115200
 *
 * WIRING (LISTEN ONLY - do not drive the bus):
 *   Arduino GND -> reader GND / key shell contact  (MUST share ground)
 *   Arduino D8  -> reader DATA / key centre contact
 *   Leave Arduino 5V disconnected from the reader. The reader supplies
 *   the pull-up and the power. A real key (original or clone) must be
 *   touching the reader while you sniff, or the reader sends nothing.
 * ------------------------------------------------------------
 *
 * HOW TO READ THE OUTPUT
 *   - "Reset"      : reader pulled the line low >300us to start a transaction.
 *   - "Presence"   : the key answered. Its width (us) is the timing fingerprint;
 *                    compare original vs clone here.
 *   - "CMD"        : the first byte after presence = the ROM command. This is
 *                    the byte that answers your question:
 *                       0x33 Read ROM      0xF0 Search ROM
 *                       0x55 Match ROM     0xCC Skip ROM
 *                       0xD1 / 0xD5        RW1990 write/flag  <-- anti-clone probe!
 *   - "bytes"      : only trustworthy after 0x33 (Read ROM), where 64 bits flow
 *                    straight. After 0xF0 (Search ROM) each ROM bit takes 3 slots,
 *                    so the byte view is meaningless - use the RAW dump instead.
 *   - "RAW us"     : every low-pulse width in order. Use this for timing analysis
 *                    and for search transactions.
 */

#define TAP_PIN_MASK _BV(PB0)   // D8 = PB0 on Uno/Nano
#define MAX_PULSES 300

// A genuine 1-Wire reset+presence looks like this. Anything outside these
// windows is floating-pin noise (uniform ~500us trains, 5012us timeout
// floods, 65535us capped resets) and gets discarded instead of printed.
#define RESET_MIN     380    // real reset low is ~480-960us
#define RESET_MAX    2000
#define PRESENCE_MIN   40    // DS1990A presence is ~60-240us
#define PRESENCE_MAX  350

uint16_t lowPulses[MAX_PULSES];
uint16_t nPulses = 0;

void setup() {
  Serial.begin(115200);
  // INPUT_PULLUP (~30-50k) holds the tap HIGH when it isn't bridged to a live
  // bus, so a disconnected/idle probe stops emitting phantom transactions.
  // It's far weaker than the reader's ~1-5k pull-up and its active low drive,
  // so it does not disturb a real transaction.
  pinMode(8, INPUT_PULLUP);
  Serial.println(F("1-Wire sniffer ready. Touch a key to the real reader..."));
}

// Rollover-safe: unsigned subtraction handles micros() wrap.
static inline uint32_t elapsed(uint32_t since) { return micros() - since; }

void loop() {
  // 1) Wait for a reset: line low for > 300us.
  while ((PINB & TAP_PIN_MASK) != 0);          // wait for LOW edge
  uint32_t start = micros();
  while ((PINB & TAP_PIN_MASK) == 0);          // wait for HIGH
  uint32_t resetLow = elapsed(start);
  if (resetLow <= 300) return;                 // not a reset, ignore

  // 2) Capture every following low-pulse width until the bus is idle.
  nPulses = 0;
  lowPulses[nPulses++] = (uint16_t)min(resetLow, 65535UL);

  while (nPulses < MAX_PULSES) {
    uint32_t idle = micros();
    while ((PINB & TAP_PIN_MASK) != 0) {       // wait for next LOW
      if (elapsed(idle) > 2500) { decodeTraffic(); return; }  // bus idle -> done
    }
    start = micros();
    while ((PINB & TAP_PIN_MASK) == 0) {       // measure the LOW
      if (elapsed(start) > 5000) break;        // safety (another reset / stuck line)
    }
    lowPulses[nPulses++] = (uint16_t)min(elapsed(start), 65535UL);
  }
  decodeTraffic();
}

const char *cmdName(uint8_t c) {
  switch (c) {
    case 0x33: return "Read ROM";
    case 0x0F: return "Read ROM (alt)";
    case 0xF0: return "Search ROM";
    case 0xEC: return "Alarm Search";
    case 0x55: return "Match ROM";
    case 0xCC: return "Skip ROM";
    case 0xD1: return "RW1990 write-flag  <== anti-clone probe";
    case 0xD5: return "RW1990 write-data  <== anti-clone probe";
    case 0xA5: return "RW2004 write       <== anti-clone probe";
    default:   return "unknown";
  }
}

// A low pulse shorter than this is a '1', longer is a '0'. Real data '1' bits
// run ~8-16us and '0' bits ~30-80us, so 30 sits safely in the gap.
#define BIT_THRESHOLD 30

// Decode 8 pulses starting at index `from` into a byte (LSB first).
// Returns false if there aren't 8 pulses available.
bool decodeByte(uint16_t from, uint8_t &out) {
  if (from + 8 > nPulses) return false;
  uint8_t b = 0;
  for (uint8_t bit = 0; bit < 8; bit++) {
    if (lowPulses[from + bit] < BIT_THRESHOLD) b |= (1 << bit);  // short low = '1'
  }
  out = b;
  return true;
}

// True if a reset+presence pair starts at index i (i.e. a sub-transaction).
bool isResetAt(uint16_t i) {
  if (i + 1 >= nPulses) return false;
  return lowPulses[i]     >= RESET_MIN    && lowPulses[i]     <= RESET_MAX &&
         lowPulses[i + 1] >= PRESENCE_MIN && lowPulses[i + 1] <= PRESENCE_MAX;
}

void printHex(uint8_t b) {
  if (b < 0x10) Serial.print('0');
  Serial.print(b, HEX);
}

// Dallas/Maxim 1-Wire CRC8 (poly 0x31, reflected -> 0x8C). CRC of the first
// 7 ROM bytes must equal the 8th byte for the code to be valid.
uint8_t crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0;
  for (uint8_t i = 0; i < len; i++) {
    uint8_t inbyte = data[i];
    for (uint8_t j = 0; j < 8; j++) {
      uint8_t mix = (crc ^ inbyte) & 0x01;
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      inbyte >>= 1;
    }
  }
  return crc;
}

// Decode one sub-transaction that starts at index `start`. Returns its command
// byte (or -1 if it couldn't be read). A real reader fires several
// reset->command attempts in one burst; each is decoded separately.
int16_t decodeSub(uint8_t n, uint16_t start) {
  uint8_t cmd;
  bool haveCmd = decodeByte(start + 2, cmd);

  Serial.print(F("  [")); Serial.print(n); Serial.print(F("] reset="));
  Serial.print(lowPulses[start]);
  Serial.print(F("us pres=")); Serial.print(lowPulses[start + 1]); Serial.print(F("us  "));
  if (!haveCmd) { Serial.println(F("CMD=? (too short)")); return -1; }

  Serial.print(F("CMD=0x")); printHex(cmd);
  Serial.print(F(" (")); Serial.print(cmdName(cmd)); Serial.println(F(")"));

  if (cmd == 0x33 || cmd == 0x0F) {
    uint8_t rom[8];
    bool full = true;
    for (uint8_t k = 0; k < 8; k++)
      if (!decodeByte(start + 10 + k * 8, rom[k])) { full = false; break; }
    if (full) {
      Serial.print(F("      ROM="));
      for (uint8_t k = 0; k < 8; k++) { printHex(rom[k]); Serial.print(' '); }
      Serial.print(F(" CRC="));
      Serial.println(crc8(rom, 7) == rom[7] ? F("PASS") : F("FAIL"));
    } else {
      Serial.println(F("      ROM=(incomplete - contact lost mid-read)"));
    }
  }
  return cmd;
}

void decodeTraffic() {
  if (!isResetAt(0)) return;   // noise gate: must open with a real reset+presence

  Serial.println(F("\n===== touch (burst of attempts) ====="));
  uint8_t n = 0;
  bool probed = false;
  uint16_t i = 0;
  while (i + 1 < nPulses) {
    if (!isResetAt(i)) { i++; continue; }
    uint16_t j = i + 2;
    while (j + 1 < nPulses && !isResetAt(j)) j++;   // next attempt / end
    int16_t cmd = decodeSub(++n, i);
    if (cmd == 0xD1 || cmd == 0xD5 || cmd == 0xA5) probed = true;
    i = j;
  }

  if (probed)
    Serial.println(F(">>> ANTI-CLONE PROBE: reader issued an RW1990 write command."
                     " A real DS1990A ignores it; an RW1990 reacts and is detected."));

  // Raw pulse widths for manual timing analysis.
  Serial.print(F("RAW us (")); Serial.print(nPulses); Serial.print(F("): "));
  for (uint16_t k = 0; k < nPulses; k++) {
    Serial.print(lowPulses[k]);
    Serial.print(k + 1 < nPulses ? ',' : '\n');
  }

  delay(150);   // let a bounced/re-touched key settle before the next capture
}
