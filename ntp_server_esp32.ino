#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <Wire.h>
#include <NTPClient.h>
#include <TinyGPSPlus.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =============================================================================
// CONFIGURATION
// =============================================================================

// Set to false to disable serial debug output
#define DEBUG_MODE false

// --- Ethernet (W5500) --------------------------------------------------------
#define PIN_ETH_CS   14
#define PIN_ETH_SCK  27
#define PIN_ETH_MISO 26
#define PIN_ETH_MOSI 25

#define ETH_SPI_FREQUENCY 8000000

byte ETH_MAC[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

IPAddress ETH_IP     (10, 0, 0, 13);
IPAddress ETH_GATEWAY(10, 0, 0,  1);
IPAddress ETH_SUBNET (255, 255, 0, 0);
IPAddress ETH_DNS    (10, 0, 0,  1);

// --- GPS (NEO-6M) ------------------------------------------------------------
#define PIN_GPS_RX 16
#define PIN_GPS_TX 17

#define GPS_BAUD_DEFAULT 9600
#define GPS_BAUD_TARGET  115200

// Navigation update rate sent to the module via UBX-CFG-RATE.
// 200 ms = 5 Hz. Higher rates do not improve timing accuracy when PPS is used.
#define GPS_UPDATE_RATE_MS 200

// Minimum plausible Unix timestamp (2023-11-14) used to reject bogus GPS dates
#define GPS_MIN_VALID_UNIX 1700000000UL

// --- PPS ---------------------------------------------------------------------
#define PIN_PPS                32
#define PPS_STALE_THRESHOLD_US 1100000UL

// Measured NMEA arrival delay after PPS pulse at 115200 baud: ~195 ms.
// Increase if NTP offset is consistently positive, decrease if negative.
#define PPS_NMEA_DELAY_US 195000UL

// --- OLED (SSD1306, 128x64) --------------------------------------------------
#define PIN_OLED_SDA 23
#define PIN_OLED_SCL 18

#define OLED_WIDTH       128
#define OLED_HEIGHT       64
#define OLED_RESET        -1
#define OLED_I2C_ADDR   0x3C
#define OLED_LINE_HEIGHT  10

// --- NTP ---------------------------------------------------------------------
#define NTP_PORT        123
#define NTP_PACKET_SIZE  48

static const uint32_t NTP_EPOCH_OFFSET = 2208988800UL;

#define NTP_PRECISION_WITH_PPS    0xEC
#define NTP_PRECISION_WITHOUT_PPS 0xF6
#define NTP_DISPERSION_WITH_PPS    0x08
#define NTP_DISPERSION_WITHOUT_PPS 0x50

// Display refreshes once per second to minimise I2C/SPI overhead in the loop
#define DISPLAY_REFRESH_MS 1000

// =============================================================================
// GLOBAL OBJECTS
// =============================================================================

HardwareSerial gpsSerial(1);
TinyGPSPlus    gps;

EthernetUDP ntpServerUDP;
EthernetUDP ntpFallbackUDP;
NTPClient   ntpFallbackClient(ntpFallbackUDP, "pool.ntp.org", 0, 5000);

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);

static uint8_t           ntpPacketBuffer[NTP_PACKET_SIZE];
static volatile uint32_t currentUnixTime   = 0;
static String            currentTimeSource = "NONE";
static unsigned long     lastDisplayUpdate = 0;

// PPS — written in ISR, read in main loop
static volatile unsigned long ppsCaptureMicros = 0;
static volatile bool          ppsIsValid       = false;

// =============================================================================
// PPS INTERRUPT
// =============================================================================

void IRAM_ATTR handlePpsInterrupt() {
  ppsCaptureMicros = micros();
  ppsIsValid       = true;
}

// =============================================================================
// OLED
// =============================================================================

static void oledPrintLine(int line, const String& text) {
  display.setCursor(0, line * OLED_LINE_HEIGHT);
  display.println(text);
}

static void updateDisplay(const String& line1,
                          const String& line2,
                          const String& line3,
                          const String& line4,
                          const String& line5 = "",
                          const String& line6 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  oledPrintLine(0, line1);
  oledPrintLine(1, line2);
  oledPrintLine(2, line3);
  oledPrintLine(3, line4);
  oledPrintLine(4, line5);
  oledPrintLine(5, line6);
  display.display();
}

// =============================================================================
// GPS HELPERS
// =============================================================================

static bool isGpsTimeValid() {
  return gps.date.isValid()
      && gps.time.isValid()
      && gps.date.year()   >= 2024
      && gps.date.month()  >= 1  && gps.date.month()  <= 12
      && gps.date.day()    >= 1  && gps.date.day()    <= 31
      && gps.time.hour()   <= 23
      && gps.time.minute() <= 59
      && gps.time.second() <= 60;
}

static uint32_t gpsToEpoch() {
  struct tm t = {};
  t.tm_year = gps.date.year()  - 1900;
  t.tm_mon  = gps.date.month() - 1;
  t.tm_mday = gps.date.day();
  t.tm_hour = gps.time.hour();
  t.tm_min  = gps.time.minute();
  t.tm_sec  = gps.time.second();

  time_t result = mktime(&t);
  return (result > (time_t)GPS_MIN_VALID_UNIX) ? (uint32_t)result : 0;
}

// =============================================================================
// TIME SOURCE
// =============================================================================

static bool refreshTime() {
  if (isGpsTimeValid() && gps.location.isValid()) {
    uint32_t t = gpsToEpoch();
    if (t > 0) {
      currentUnixTime   = t;
      currentTimeSource = "GPS";
      if (DEBUG_MODE) Serial.printf("[TIME] GPS epoch: %lu\n", currentUnixTime);
      return true;
    }
  }

  ntpFallbackClient.update();
  if (ntpFallbackClient.isTimeSet()) {
    currentUnixTime   = ntpFallbackClient.getEpochTime();
    currentTimeSource = "NTP";
    if (DEBUG_MODE) Serial.printf("[TIME] NTP fallback epoch: %lu\n", currentUnixTime);
    return true;
  }

  currentUnixTime   = 0;
  currentTimeSource = "NONE";
  if (DEBUG_MODE) Serial.println("[TIME] No valid time source");
  return false;
}

// Returns the current time as a 64-bit NTP timestamp.
// With PPS: sub-second fraction is interpolated from micros() elapsed since
//           the last pulse, compensated for the measured NMEA arrival delay.
// Without:  falls back to micros() % 1000000 — rough estimate only.
static uint64_t buildNtpTimestamp() {
  // Atomic snapshot — prevents ISR from corrupting the pair mid-read
  portDISABLE_INTERRUPTS();
  const unsigned long captureMicros = ppsCaptureMicros;
  const bool          hasPps        = ppsIsValid;
  const uint32_t      epochSnapshot = currentUnixTime;
  portENABLE_INTERRUPTS();

  uint32_t epochSeconds;
  uint32_t microsIntoSecond;

  if (hasPps) {
    unsigned long elapsed = micros() - captureMicros;
    if (elapsed < PPS_STALE_THRESHOLD_US) {
      unsigned long compensated = (elapsed >= PPS_NMEA_DELAY_US)
                                  ? elapsed - PPS_NMEA_DELAY_US
                                  : 0;
      uint32_t fullSeconds = (uint32_t)(compensated / 1000000UL);
      epochSeconds     = epochSnapshot + fullSeconds;
      microsIntoSecond = compensated % 1000000UL;
    } else {
      epochSeconds     = epochSnapshot;
      microsIntoSecond = micros() % 1000000UL;
    }
  } else {
    epochSeconds     = epochSnapshot;
    microsIntoSecond = micros() % 1000000UL;
  }

  uint64_t seconds1900 = (uint64_t)epochSeconds + NTP_EPOCH_OFFSET;
  uint32_t fraction    = (uint32_t)((double)microsIntoSecond * 4294.967296);

  return (seconds1900 << 32) | fraction;
}

// =============================================================================
// BINARY WRITE HELPERS
// =============================================================================

static void writeU32BE(uint8_t* buf, int offset, uint32_t value) {
  buf[offset + 0] = (value >> 24) & 0xFF;
  buf[offset + 1] = (value >> 16) & 0xFF;
  buf[offset + 2] = (value >>  8) & 0xFF;
  buf[offset + 3] =  value        & 0xFF;
}

static void writeU64BE(uint8_t* buf, int offset, uint64_t value) {
  buf[offset + 0] = (value >> 56) & 0xFF;
  buf[offset + 1] = (value >> 48) & 0xFF;
  buf[offset + 2] = (value >> 40) & 0xFF;
  buf[offset + 3] = (value >> 32) & 0xFF;
  buf[offset + 4] = (value >> 24) & 0xFF;
  buf[offset + 5] = (value >> 16) & 0xFF;
  buf[offset + 6] = (value >>  8) & 0xFF;
  buf[offset + 7] =  value        & 0xFF;
}

// =============================================================================
// NTP SERVER
// =============================================================================

static void sendNtpResponse(IPAddress remoteIp,
                            uint16_t  remotePort,
                            uint8_t*  requestBuffer) {
  memset(ntpPacketBuffer, 0, NTP_PACKET_SIZE);

  const uint64_t receiveTime  = buildNtpTimestamp();
  const uint64_t transmitTime = buildNtpTimestamp();

  const bool usingGps = (currentTimeSource == "GPS");
  const bool usingPps = ppsIsValid;

  ntpPacketBuffer[0] = 0b00100100;
  ntpPacketBuffer[1] = usingGps ? 1 : 2;
  ntpPacketBuffer[2] = 4;
  ntpPacketBuffer[3] = usingPps ? NTP_PRECISION_WITH_PPS : NTP_PRECISION_WITHOUT_PPS;

  writeU32BE(ntpPacketBuffer, 4, 0);
  writeU32BE(ntpPacketBuffer, 8,
             usingPps ? NTP_DISPERSION_WITH_PPS : NTP_DISPERSION_WITHOUT_PPS);

  ntpPacketBuffer[12] = usingGps ? 'G' : 'N';
  ntpPacketBuffer[13] = usingGps ? 'P' : 'T';
  ntpPacketBuffer[14] = usingGps ? 'S' : 'P';
  ntpPacketBuffer[15] = 0;

  writeU64BE(ntpPacketBuffer, 16, transmitTime);
  memcpy(&ntpPacketBuffer[24], &requestBuffer[40], 8);
  writeU64BE(ntpPacketBuffer, 32, receiveTime);
  writeU64BE(ntpPacketBuffer, 40, transmitTime);

  ntpServerUDP.beginPacket(remoteIp, remotePort);
  ntpServerUDP.write(ntpPacketBuffer, NTP_PACKET_SIZE);
  ntpServerUDP.endPacket();

  if (DEBUG_MODE) {
    Serial.printf("[NTP] Response → %s:%u  source=%s  pps=%s\n",
                  remoteIp.toString().c_str(), remotePort,
                  currentTimeSource.c_str(), usingPps ? "yes" : "no");
  }
}

static void processNtpRequests() {
  int packetSize = ntpServerUDP.parsePacket();

  if (packetSize == NTP_PACKET_SIZE) {
    IPAddress remoteIp   = ntpServerUDP.remoteIP();
    uint16_t  remotePort = ntpServerUDP.remotePort();

    uint8_t requestBuffer[NTP_PACKET_SIZE];
    ntpServerUDP.read(requestBuffer, NTP_PACKET_SIZE);

    sendNtpResponse(remoteIp, remotePort, requestBuffer);

  } else if (packetSize > 0) {
    ntpServerUDP.flush();
    if (DEBUG_MODE) {
      Serial.printf("[NTP] Discarded malformed packet (%d bytes)\n", packetSize);
    }
  }
}

// =============================================================================
// UBX (u-blox binary protocol)
// =============================================================================

static void sendUbxFrame(const uint8_t* frame, size_t length) {
  uint8_t ckA = 0, ckB = 0;
  for (size_t i = 2; i < length; i++) {
    ckA += frame[i];
    ckB += ckA;
  }
  gpsSerial.write(frame, length);
  gpsSerial.write(ckA);
  gpsSerial.write(ckB);
  gpsSerial.flush();
}

static void setGpsBaudRate(uint32_t targetBaud) {
  uint8_t frame[] = {
    0xB5, 0x62,
    0x06, 0x00,
    0x14, 0x00,
    0x01,
    0x00,
    0x00, 0x00,
    0xD0, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x07, 0x00,
    0x03, 0x00,
    0x00, 0x00,
    0x00, 0x00
  };

  frame[12] = (targetBaud)       & 0xFF;
  frame[13] = (targetBaud >>  8) & 0xFF;
  frame[14] = (targetBaud >> 16) & 0xFF;
  frame[15] = (targetBaud >> 24) & 0xFF;

  sendUbxFrame(frame, sizeof(frame));
  delay(100);

  gpsSerial.begin(targetBaud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  delay(200);
  while (gpsSerial.available()) gpsSerial.read();

  unsigned long waitStart = millis();
  bool cleanStart = false;
  while (millis() - waitStart < 2000) {
    if (gpsSerial.available() && gpsSerial.peek() == '$') { cleanStart = true; break; }
    if (gpsSerial.available()) gpsSerial.read();
  }

  if (DEBUG_MODE) {
    Serial.println(cleanStart ? "[GPS] Clean NMEA start at new baud"
                              : "[GPS] WARNING: no clean NMEA after baud switch");
    Serial.printf("[GPS] Baud rate → %lu\n", targetBaud);
  }
}

static void setGpsUpdateRate(uint16_t intervalMs) {
  uint8_t frame[] = {
    0xB5, 0x62,
    0x06, 0x08,
    0x06, 0x00,
    0x00, 0x00,
    0x01, 0x00,
    0x01, 0x00
  };

  frame[6] = (intervalMs)      & 0xFF;
  frame[7] = (intervalMs >> 8) & 0xFF;

  sendUbxFrame(frame, sizeof(frame));
  delay(100);

  if (DEBUG_MODE) {
    Serial.printf("[GPS] Update rate → %u ms (%u Hz)\n",
                  intervalMs, 1000u / intervalMs);
  }
}

// =============================================================================
// SETUP ROUTINES
// =============================================================================

static void setupOled() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    if (DEBUG_MODE) Serial.println("[OLED] Initialisation failed");
    return;
  }

  display.clearDisplay();
  display.display();
  updateDisplay("ETH GPS NTP", "Booting...", "", "", "", "");
  if (DEBUG_MODE) Serial.println("[OLED] Ready");
}

static void setupEthernet() {
  updateDisplay("ETH GPS NTP", "ETH connecting...", "", "", "", "");

  pinMode(PIN_ETH_CS, OUTPUT);
  digitalWrite(PIN_ETH_CS, HIGH);

  SPI.begin(PIN_ETH_SCK, PIN_ETH_MISO, PIN_ETH_MOSI, PIN_ETH_CS);
  SPI.setFrequency(ETH_SPI_FREQUENCY);

  Ethernet.init(PIN_ETH_CS);
  Ethernet.begin(ETH_MAC, ETH_IP, ETH_DNS, ETH_GATEWAY, ETH_SUBNET);
  delay(1000);

  if (DEBUG_MODE) {
    Serial.print("[ETH] IP address: ");
    Serial.println(Ethernet.localIP());
  }
}

static void setupGps() {
  // Probe GPS_BAUD_TARGET first — module persists baud rate across ESP32 resets
  if (DEBUG_MODE) Serial.printf("[GPS] Probing %d baud...\n", GPS_BAUD_TARGET);
  gpsSerial.begin(GPS_BAUD_TARGET, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  delay(300);
  while (gpsSerial.available()) gpsSerial.read();

  unsigned long t = millis();
  bool found = false;
  while (millis() - t < 1000) {
    if (gpsSerial.available() && gpsSerial.peek() == '$') { found = true; break; }
    if (gpsSerial.available()) gpsSerial.read();
  }

  if (found) {
    if (DEBUG_MODE) Serial.printf("[GPS] Already at %d baud\n", GPS_BAUD_TARGET);
    goto done;
  }

  // Fall back to default baud and switch via UBX
  if (DEBUG_MODE) Serial.printf("[GPS] Probing %d baud...\n", GPS_BAUD_DEFAULT);
  gpsSerial.begin(GPS_BAUD_DEFAULT, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  delay(300);
  while (gpsSerial.available()) gpsSerial.read();

  t = millis();
  found = false;
  while (millis() - t < 2000) {
    if (gpsSerial.available() && gpsSerial.peek() == '$') { found = true; break; }
    if (gpsSerial.available()) gpsSerial.read();
  }

  if (!found) {
    if (DEBUG_MODE) Serial.println("[GPS] WARNING: no NMEA at either baud — check wiring");
    goto done;
  }

  if (DEBUG_MODE) Serial.println("[GPS] NMEA confirmed at 9600 — switching via UBX");
  setGpsBaudRate(GPS_BAUD_TARGET);

done:
  delay(200);
  while (gpsSerial.available()) gpsSerial.read();

  setGpsUpdateRate(GPS_UPDATE_RATE_MS);

  pinMode(PIN_PPS, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_PPS), handlePpsInterrupt, RISING);
  if (DEBUG_MODE) Serial.printf("[GPS] PPS interrupt attached on GPIO %d\n", PIN_PPS);
}

// =============================================================================
// DISPLAY HELPER
// =============================================================================

static String buildTimeString() {
  if (!isGpsTimeValid()) return "No GPS time";

  // Without PPS the millisecond field is an estimate — mark with tilde
  uint32_t ms      = ppsIsValid
                     ? ((micros() - ppsCaptureMicros) / 1000UL) % 1000UL
                     : millis() % 1000UL;
  bool     precise = ppsIsValid;

  char buf[24];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d%s",
           gps.time.hour(), gps.time.minute(), gps.time.second(),
           ms, precise ? "" : "~");
  return String(buf);
}

// =============================================================================
// ARDUINO ENTRY POINTS
// =============================================================================

void setup() {
  if (DEBUG_MODE) {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] ESP32 ETH GPS NTP Server starting");
  }

  setupOled();
  setupGps();
  setupEthernet();

  ntpFallbackClient.begin();
  ntpFallbackClient.setTimeOffset(0);
  ntpFallbackClient.forceUpdate();

  ntpServerUDP.begin(NTP_PORT);

  updateDisplay(
    "ETH GPS NTP",
    Ethernet.localIP().toString(),
    "Port: 123",
    "Waiting for GPS...",
    "", ""
  );

  if (DEBUG_MODE) Serial.println("[NTP] Server listening on UDP port 123");
}

void loop() {
  // Feed all available GPS bytes into the NMEA parser
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // Handle NTP requests as fast as possible — no blocking calls above this
  processNtpRequests();

  // Refresh time source and display once per second
  if (millis() - lastDisplayUpdate >= DISPLAY_REFRESH_MS) {
    lastDisplayUpdate = millis();

    refreshTime();

    const String ip        = Ethernet.localIP().toString();
    const String gpsStatus = isGpsTimeValid() ? "GPS OK" : "GPS WAIT";
    const String sats      = gps.satellites.isValid()
                               ? String(gps.satellites.value()) : "-";
    const String hdop      = gps.hdop.isValid()
                               ? String(gps.hdop.hdop(), 2)     : "-";
    const String timeStr   = buildTimeString();

    updateDisplay(
      "ETH GPS NTP",
      ip,
      gpsStatus + "  SAT: " + sats,
      "HDOP: " + hdop,
      "SRC: " + currentTimeSource,
      timeStr
    );

    if (DEBUG_MODE) {
      Serial.printf("[LOOP] %s | %s | SAT:%s | HDOP:%s | %s | PPS:%s\n",
                    ip.c_str(), gpsStatus.c_str(), sats.c_str(),
                    hdop.c_str(), timeStr.c_str(), ppsIsValid ? "yes" : "no");
    }
  }
}
