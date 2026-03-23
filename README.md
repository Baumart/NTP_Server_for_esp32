# ESP32 GPS NTP Server

A Stratum 1 NTP server running on an ESP32 with a NEO-6M GPS module and a W5500 Ethernet adapter.  
Time is disciplined by the GPS PPS (Pulse Per Second) signal for sub-millisecond accuracy.

Thanks to [Stuart's blog](https://stuartsprojects.github.io/2024/09/21/How-not-to-read-a-GPS.html) for GPS parsing guidance.  
If Windows refuses to sync to a non-domain NTP server, see [this Microsoft article](https://learn.microsoft.com/en-us/troubleshoot/windows-server/active-directory/time-synchronization-not-succeed-non-ntp).

## 📜 Reference

NTP protocol: [RFC 5905](https://datatracker.ietf.org/doc/html/rfc5905)  
UBX protocol: [u-blox 6 Receiver Description](https://content.u-blox.com/sites/default/files/products/documents/u-blox6_ReceiverDescrProtSpec_%28GPS.G6-SW-10018%29_Public.pdf)

---

## 📡 Hardware

| Component | Details |
|---|---|
| Microcontroller | Wemos Lolin ESP32 |
| GPS module | GPS6MV2 (u-blox NEO-6M) |
| Ethernet | W5500 module |
| Display | SSD1306 OLED 128×64 |

### Wiring

**W5500 Ethernet**
| W5500 | ESP32 GPIO |
|---|---|
| CS | 14 |
| SCK | 27 |
| MISO | 26 |
| MOSI | 25 |

**GPS (NEO-6M)**
| GPS | ESP32 GPIO |
|---|---|
| TX | 16 (RX) |
| RX | 17 (TX) |
| PPS / TIMEPULSE pad | 32 |

> **PPS wiring:** The NEO-6M breakout board does not expose the PPS pin in the standard 4-pin header.  
> Solder a thin wire to the `TIMEPULSE` test pad on the PCB (connected to the blinking status LED)  
> and connect it to GPIO 32 on the ESP32.

**OLED (I2C)**
| OLED | ESP32 GPIO |
|---|---|
| SDA | 23 |
| SCL | 18 |

---

## 🔧 Configuration

All settings are at the top of `eth_ntp_server.ino`:

```cpp
// Network
IPAddress ETH_IP     (10, 0, 0, 13);
IPAddress ETH_GATEWAY(10, 0, 0,  1);
IPAddress ETH_SUBNET (255, 255, 0, 0);
IPAddress ETH_DNS    (10, 0, 0,  1);

// GPS
#define GPS_BAUD_TARGET    115200   // target baud after UBX config
#define GPS_UPDATE_RATE_MS 200      // 200 ms = 5 Hz

// PPS
#define PIN_PPS 32

// Debug serial output
#define DEBUG_MODE true
```

---

## 🛠 Features

- **Stratum 1** NTP server when GPS lock is active
- **PPS discipline** — microseconds interpolated from hardware pulse, ~1 µs accuracy
- **UBX auto-configuration** — switches NEO-6M from 9600 to 115200 baud and sets 5 Hz update rate on boot, no u-center needed
- **Graceful fallback chain**: GPS+PPS → GPS only → NTP pool (pool.ntp.org) → no time
- **NTP quality fields** adjust automatically: precision and root dispersion reflect whether PPS is active
- **OLED display** shows IP, GPS status, satellite count, HDOP, time source, and current time with milliseconds
- **Malformed packet flushing** prevents UDP buffer back-up

### OLED layout

```
ETH GPS NTP
10.0.0.13
GPS OK  SAT: 8
HDOP: 0.90
SRC: GPS
14:32:11.347        ← with PPS (accurate)
14:32:11.347~       ← without PPS (estimated)
```

### Debug serial output

Set `DEBUG_MODE true` to see structured output over USB serial at 115200 baud:

```
[BOOT] ESP32 ETH GPS NTP Server starting
[OLED] Ready
[GPS]  Serial started at 9600 baud
[GPS]  Baud rate → 115200
[GPS]  Update rate → 200 ms (5 Hz)
[GPS]  PPS interrupt attached on GPIO 32
[ETH]  IP address: 10.0.0.13
[NTP]  Server listening on UDP port 123
[TIME] GPS epoch: 1711234567
[NTP]  Response → 10.0.0.5:123  source=GPS  pps=yes
[LOOP] 10.0.0.13 | GPS OK | SAT:8 | HDOP:0.90 | 14:32:11.347 | PPS:yes
```

---

## ⚡ Usage

1. Wire hardware as described above
2. Set your network addresses in the configuration section
3. Flash to the ESP32
4. Point your devices at `10.0.0.13` as their NTP server

**Windows (command prompt, run as administrator):**
```
w32tm /config /manualpeerlist:"10.0.0.13" /syncfromflags:manual /reliable:YES /update
net stop w32tm && net start w32tm
w32tm /resync
```

**Linux / macOS (`/etc/ntp.conf` or `chrony.conf`):**
```
server 10.0.0.13 iburst prefer
```

---

## 📌 Notes

- The GPS module needs a clear sky view to acquire a fix; first fix after power-on can take several minutes
- Without a PPS signal the millisecond display is an estimate (marked with `~`) and NTP precision degrades to Stratum 2 quality
- The MAC address in the code (`DE:AD:BE:EF:FE:ED`) is a placeholder — change it if you run multiple devices on the same network

---

## ❌ Not yet implemented

- Leap Indicator field changes
- Reference Timestamp (currently mirrors Transmit Timestamp)
- NTP Key Identifier and Message Digest (authentication)

---

## 📄 License

Copyright 2025 — MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
