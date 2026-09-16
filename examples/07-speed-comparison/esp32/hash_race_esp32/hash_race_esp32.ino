// Dasduino CONNECTPLUS (ESP32) side of the hash race. Same math as hash_race.v:
// single-lane brute force vs the FPGA's N=4 parallel lanes.
//
// Hash is a Murmur3-style avalanche finalizer (xor-shift + odd-constant
// multiply, twice) -- must match hash_race.v's fmix32 exactly, or the two
// boards aren't racing the same problem. TARGET is picked backward in
// Python so the match lands at an exact, known candidate -- deterministic
// run length, not a statistical mean.
//
// Onboard LED on this board is a WS2812 (addressable RGB) on GPIO32,
// not a plain GPIO LED -- needs NeoPixel protocol, not digitalWrite.

#include <Adafruit_NeoPixel.h>

#define LED_PIN 32
Adafruit_NeoPixel pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);

// Shared start button, same node as FPGA's PIN_19/B8: external pullup to
// 3V3, button to GND, pressed = LOW. External pullup already present, so
// plain INPUT (no internal pullup needed).
#define BTN_PIN 14

const uint32_t TARGET = 0x0441D1D5; // = fmix32(410,000,000)

static inline uint32_t fmix32(uint32_t h) {
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

uint32_t find_candidate() {
  uint32_t candidate = 0;
  while (fmix32(candidate) != TARGET) candidate++;
  return candidate;
}

void setup() {
  pinMode(BTN_PIN, INPUT);
  pixel.begin();
  pixel.setPixelColor(0, 0);
  pixel.show();
  Serial.begin(115200);
  delay(200);

  Serial.println("waiting for start button...");
  while (digitalRead(BTN_PIN) == HIGH) {
    // block until the shared button pulls this low
  }

  Serial.println("searching...");
  unsigned long t0 = micros();

  uint32_t candidate = find_candidate();

  unsigned long elapsed_us = micros() - t0;
  pixel.setPixelColor(0, pixel.Color(0, 255, 0));
  pixel.show();

  Serial.printf("found candidate=%lu after %lu.%03lu s\n",
                (unsigned long)candidate,
                elapsed_us / 1000000UL,
                (elapsed_us / 1000UL) % 1000UL);
}

void loop() {
  // done, LED stays on
}
