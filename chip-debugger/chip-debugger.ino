/*
 * 1-Wire Passive Command Sniffer
 * Pin: D8 (PB0 on Uno/Nano)
 * Baud: 115200
 */

#define MAX_PULSES 256
volatile uint16_t lowPulses[MAX_PULSES];
volatile uint16_t bitCount = 0;

void setup() {
  Serial.begin(115200);
  pinMode(8, INPUT); // High-Z mode (passive tap)
  Serial.println("Ready. Touch key fob or tap scanner probe...");
}

void loop() {
  // 1. Wait for Reset Pulse (Line held LOW for > 300us)
  while ((PINB & _BV(PB0)) != 0); // Wait for LOW
  unsigned long start = micros();
  while ((PINB & _BV(PB0)) == 0); // Wait for HIGH
  unsigned long duration = micros() - start;

  if (duration > 300) { // Valid 1-Wire Reset
    bitCount = 0;
    lowPulses[bitCount++] = duration;

    // 2. Sample incoming low pulses until line goes idle (> 2000us)
    while (bitCount < MAX_PULSES) {
      unsigned long timeout = micros();
      
      // Wait for next LOW edge
      while ((PINB & _BV(PB0)) != 0) {
        if (micros() - timeout > 2000) goto dump_data; // Idle bus
      }
      
      start = micros();
      // Measure LOW pulse duration
      while ((PINB & _BV(PB0)) == 0) {
        if (micros() - start > 1000) break; // Timeout guard
      }
      lowPulses[bitCount++] = micros() - start;
    }

  dump_data:
    decodeTraffic();
  }
}

void decodeTraffic() {
  if (bitCount < 3) return;

  Serial.println("\n--- 1-Wire Transaction Captured ---");
  Serial.print("Reset Pulse: "); Serial.print(lowPulses[0]); Serial.println(" us");

  uint8_t currentByte = 0;
  uint8_t bitPos = 0;

  Serial.print("Decoded Bytes: ");

  for (uint16_t i = 1; i < bitCount; i++) {
    uint16_t w = lowPulses[i];

    // Presence Pulse from tag (~60uS - 240us)
    if (i == 1 && w >= 40 && w <= 300) {
      Serial.print("[Presence: "); Serial.print(w); Serial.print("us] ");
      continue;
    }

    // 1-Wire bit decoding: Short pulse (<20us) = '1', Long pulse (>30us) = '0'
    bool bitVal = (w < 25);

    if (bitVal) {
      currentByte |= (1 << bitPos);
    }

    bitPos++;
    if (bitPos == 8) {
      if (currentByte < 0x10) Serial.print("0");
      Serial.print(currentByte, HEX);
      Serial.print(" ");

      currentByte = 0;
      bitPos = 0;
    }
  }
  Serial.println();
  delay(200); 
}