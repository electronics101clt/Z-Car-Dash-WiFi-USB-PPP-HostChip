# Z-Car-Dashboard WiFi PPP Modem

ESP32-based car dashboard with integrated Hayes AT command modem and PPP server for wireless Internet connectivity over USB serial.

## Overview

This project combines a full-featured car dashboard with a WiFi-to-serial modem, allowing:
- Real-time OBD-II data display (speed, RPM, coolant temp, voltage, etc.)
- Remote car control (lock/unlock, remote start, alarm)
- **Hayes AT modem emulation** over USB serial (74880 baud)
- **PPP server** for dialup-style Internet access through WiFi
- Simultaneous AP+STA mode (hosts dashboard while routing Internet traffic)

## Features

### Car Dashboard
- **Animated gauge display** - Speed, RPM, coolant temp, voltage, throttle, fuel level, engine load, intake temp, MAP pressure
- **OBD-II Bluetooth LE** - Connects to ELM327 adapters
- **MPG tracking** - Fill-to-fill average, economy score, miles till empty
- **Diagnostic codes** - Read and clear DTCs
- **Remote control** - Lock/unlock doors, remote start engine, dome light control
- **Web interface** - Accessible at `http://192.168.4.1/` (WiFi) or `http://10.0.0.1/` (PPP)

### WiFi Modem
- **Hayes AT commands** - Industry-standard modem command set
- **PPP server** - Full Point-to-Point Protocol implementation with LCP/IPCP negotiation
- **Phonebook system** - Store WiFi credentials mapped to "phone numbers"
- **Binary-safe transmission** - Fixes null byte corruption in base64-encoded images
- **56K modem emulation** - Appears as `/dev/ttyUSB0` on Linux

## Hardware Requirements

- **ESP32 DevKit** (standard ESP32, not S2/S3/C3)
- **USB-UART bridge** (CP2102, CH340, or similar - built into most dev boards)
- **ELM327 Bluetooth LE adapter** (optional, for OBD-II data)
- **Relay module** (optional, for remote start/alarm control)

⚠️ **IMPORTANT: This is a LARGE application** - The compiled sketch uses **~1,282,536 bytes (40%) of program storage space** with the **Huge APP** partition scheme (3,145,728 byte maximum). You must use the "Huge APP (3MB No OTA/1MB SPIFFS)" partition scheme when flashing, otherwise the default partition (1.3MB) will not have enough space and compilation will fail or the app will be severely limited.

## Installation

### Flash Firmware

**Using Arduino IDE:**
1. Install ESP32 board support: https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html
2. Install NimBLE-Arduino library: `Sketch → Include Library → Manage Libraries → search "NimBLE-Arduino"`
3. Open `Z-Car-Dashboard.ino`
4. Select `Tools → Board → ESP32 Dev Module`
5. **CRITICAL:** Select `Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)`
6. Select correct COM port
7. Click Upload

**Using arduino-cli:**
```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app Z-Car-Dashboard.ino
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app Z-Car-Dashboard.ino
```

**Note:** The `:PartitionScheme=huge_app` suffix is required to use the 3MB app partition instead of the default 1.3MB.

## Usage

### Car Dashboard Mode

1. **Connect to WiFi hotspot** - SSID: `ZS-xxxxxxxx`, Password: `12345678`
2. **Open browser** - Navigate to `http://192.168.4.1/`
3. **View dashboard** - Real-time gauges update automatically
4. **Configure settings** - Click "SETTINGS" to adjust WiFi password, OBD filter, MPG tracking

### Modem Mode

#### Setup Phonebook

Connect to serial port (74880 baud) and add WiFi credentials:

```
AT                                      # Test connection
AT+PBADD=5551234,MyWiFi,password        # Add WiFi network
AT+PBLIST                               # List phonebook entries
```

#### Dial Into WiFi

**Using minicom:**
```bash
minicom -D /dev/ttyUSB0 -b 9600
ATDT5551234                             # Dial the number
# Modem responds: CONNECT 56000
```

**Using pppd (Internet access):**

Create `/etc/ppp/peers/esp32-modem`:
```
/dev/ttyUSB0
9600
crtscts
lock
noauth
defaultroute
usepeerdns
persist
connect "/usr/sbin/chat -v -f /etc/chatscripts/esp32-modem"
```

Create `/etc/chatscripts/esp32-modem`:
```
ABORT BUSY
ABORT 'NO CARRIER'
ABORT ERROR
TIMEOUT 30
'' ATZ
OK 'ATDT5551234'
CONNECT \d\c
```

Connect:
```bash
sudo pppd call esp32-modem
# Wait for connection, then:
ping 8.8.8.8                            # Test Internet
```

Access dashboard via PPP:
```bash
curl http://10.0.0.1/                   # Dashboard HTML
curl http://10.0.0.1/api/data           # OBD data JSON
```

## AT Command Reference

### Standard Hayes Commands

| Command | Description |
|---------|-------------|
| `AT` | Test command (returns OK) |
| `ATZ` | Reset modem |
| `ATE0` | Echo off |
| `ATE1` | Echo on |
| `ATV0` | Numeric responses |
| `ATV1` | Verbose responses (default) |
| `ATI` | Identification |
| `ATH` | Hangup |
| `ATDT<number>` | Dial tone (connect to WiFi) |
| `ATDP<number>` | Dial pulse (same as tone) |

### Vendor Commands (WiFi/Phonebook)

| Command | Description |
|---------|-------------|
| `AT+PBLIST` | List all phonebook entries |
| `AT+PBADD=<num>,<ssid>,<pass>` | Add phonebook entry |
| `AT+PBDEL=<number>` | Delete phonebook entry |
| `AT+WIFISTATUS` | Show WiFi connection status |
| `AT+IPR=<baudrate>` | Set DTE baud rate (volatile, resets on power cycle) |
| `AT+IPR?` | Query current baud rate |

### Examples

```
AT                              → OK
ATI                             → ESP32 WiFi Modem v1.0
                                  Z-Car-Dashboard
                                  OK
AT+IPR?                         → +IPR: 9600
                                  OK
AT+IPR=115200                   → OK
                                  (modem switches to 115200 immediately)
AT+PBADD=5551234,HomeWiFi,secret123  → OK
AT+PBLIST                       → 5551234 -> HomeWiFi
                                  OK
ATDT5551234                     → CONNECT 56000
                                  (PPP negotiation starts)
+++                             → (1 second pause, hangup)
ATH                             → NO CARRIER
```

## Architecture

### Network Topology

```
┌────────────────────────────────────────┐
│           ESP32 WiFi Router            │
│                                        │
│  WAN: WiFi STA → Internet Router       │
│                                        │
│  LAN Interfaces:                       │
│   - WiFi AP (192.168.4.1)              │
│   - Serial PPP (10.0.0.1)              │
│                                        │
│  Services:                             │
│   - WebServer (port 80)                │
│   - PPP Server (serial 74880)          │
└────────────────────────────────────────┘
         ↓                    ↓
    Phone/Tablet         Ubuntu/Linux
    (WiFi Client)        (PPP Client)
```

### Data Flow

**Dashboard Access (WiFi):**
```
Phone → WiFi AP → ESP32 WebServer → Dashboard HTML
```

**Dashboard Access (PPP):**
```
Ubuntu → Serial → PPP → ESP32 WebServer → Dashboard HTML
```

**Internet Access (PPP):**
```
Ubuntu → Serial → PPP → ESP32 NAT → WiFi STA → Internet
```

### PPP Implementation

Based on [Zimodem](https://github.com/bozimmerman/Zimodem) PPP server:
- **LCP (Link Control Protocol)** - Link establishment, configuration, testing
- **IPCP (IP Control Protocol)** - IP address negotiation
- **Frame handling** - HDLC-like framing with byte stuffing
- **FCS validation** - CRC-16 frame check sequence
- **Packet forwarding** - Bidirectional routing between Serial and WiFi via lwIP hooks

## Configuration

### WiFi Password

Change the dashboard WiFi password via web interface:
1. Connect to `http://192.168.4.1/`
2. Click "SETTINGS"
3. Enter new password
4. Click save

Default: `12345678`

### Serial Port Settings

- **Default baud rate:** 9600 (Hayes standard)
- **Data bits:** 8
- **Parity:** None
- **Stop bits:** 1
- **Flow control:** Hardware (RTS/CTS)

**Baud Rate Negotiation:**
- Modem always boots at 9600 baud (default Hayes baud rate)
- Use `AT+IPR=<rate>` to change baud rate during session
- Supported rates: 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600
- Baud rate changes are **volatile** - resets to 9600 on power cycle
- This ensures modem is always accessible at known default rate

### OBD-II Filter

Customize which Bluetooth devices to scan for:
- Default: `OBD,ELM,V-LINK,IOS-`
- Edit via web interface Settings page

## Testing and Verification

### Automated Modem Testing

You can verify modem functionality using the included expect script:

```bash
# Install expect if not already installed
sudo apt-get install expect

# Create and run test script
cat > /tmp/test_modem.exp << 'EOF'
#!/usr/bin/expect -f
set timeout 5
spawn picocom -b 9600 -q /dev/ttyUSB0

sleep 2

send "AT\r"
expect "OK"
puts "\n=== AT command test: PASSED ==="

send "ATI\r"
expect "OK"
puts "\n=== ATI identification: PASSED ==="

send "AT+IPR?\r"
expect "+IPR:"
expect "OK"
puts "\n=== AT+IPR query: PASSED ==="

send "AT+WIFISTATUS\r"
expect "AP:"
expect "STA:"
expect "OK"
puts "\n=== AT+WIFISTATUS: PASSED ==="

send "AT+PBLIST\r"
expect "OK"
puts "\n=== AT+PBLIST: PASSED ==="

puts "\n=== All modem tests PASSED! ==="
sleep 1
exit
EOF

chmod +x /tmp/test_modem.exp
/tmp/test_modem.exp
```

### Expected Test Results

**Successful modem test output:**
```
=== AT command test: PASSED ===
ESP32 WiFi Modem v1.0
Z-Car-Dashboard
=== ATI identification: PASSED ===
AT+IPR?
+IPR: 9600
=== AT+IPR query: PASSED ===
AT+WIFISTATUS
AP: ZS-xxxxxxxx
STA: Disconnected
=== AT+WIFISTATUS: PASSED ===
AT+PBLIST
=== AT+PBLIST: PASSED ===
=== All modem tests PASSED! ===
```

### WiFi Connection Test

Test WiFi connection and PPP mode:

```bash
# In your serial terminal (picocom, minicom, etc.)
AT+PBADD=5551234,YourWiFiSSID,YourPassword
AT+PBLIST
ATDT5551234

# Expected response:
CONNECT 56000
# (PPP negotiation starts)
```

**Note:** SSID is case-sensitive. Use exact capitalization.

## Troubleshooting

### Modem Not Responding

```bash
# Check serial port
ls -l /dev/ttyUSB*

# Test with picocom
picocom -b 9600 /dev/ttyUSB0
AT
# Should respond: OK

# Or use screen
screen /dev/ttyUSB0 9600
AT
# Should respond: OK
```

### PPP Won't Connect

```bash
# Enable pppd debug logging
sudo pppd call esp32-modem debug dump logfile /tmp/ppp.log

# Check logs
tail -f /tmp/ppp.log
```

### Dashboard Won't Load

- Check WiFi connection: `iwconfig`
- Verify IP address: `ip addr show wlan0`
- Test connectivity: `ping 192.168.4.1`
- Check browser console for errors

### Null Byte Corruption (Old Serial Protocol)

**Symptom:** Dashboard loads with broken images when using `GET /` serial commands

**Solution:** Use PPP mode instead of raw serial commands. PPP is binary-safe and handles base64-encoded images correctly.

## API Reference

### REST Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Dashboard HTML |
| `/api/data` | GET | OBD-II data (JSON) |
| `/api/dtc` | GET | Diagnostic trouble codes (JSON) |
| `/api/obd/status` | GET | OBD connection status |
| `/api/obd/scan` | GET | Scan for BLE devices |
| `/api/obd/connect?addr=<mac>` | GET | Connect to OBD device |
| `/api/obd/disconnect` | GET | Disconnect OBD |
| `/api/obd/settings?<params>` | GET | Update settings |
| `/api/control/lock` | GET | Lock doors |
| `/api/control/unlock` | GET | Unlock doors |
| `/api/control/domelight?state=<0\|1>` | GET | Control dome light |
| `/api/control/remotestart` | GET | Remote start engine |
| `/api/control/status` | GET | Alarm/engine status |
| `/api/mpg/reset` | GET | Reset MPG tracking data |

### Data Format

**`/api/data` Response:**
```json
{
  "speed": 65.5,
  "rpm": 2100,
  "coolant": 90.0,
  "fuel": 75.5,
  "load": 45.2,
  "voltage": 14.1,
  "intake": 35.0,
  "throttle": 25.0,
  "mapMpg": 28.5,
  "economy": 62.0,
  "tripMiles": 145.2,
  "mte": 320,
  "checkEngine": false,
  "obdConnected": true
}
```

## Security Considerations

### Default Password

⚠️ **Change the default WiFi password (`12345678`)** before deploying in a vehicle. Anyone within WiFi range can access the dashboard and remote control functions.

### Remote Start

The remote start feature activates vehicle relays. **Ensure proper wiring and safety interlocks** before enabling this feature. Improper installation can damage vehicle electronics or create safety hazards.

### API Access

The web API has **no authentication**. Anyone connected to the WiFi network (or PPP connection) can:
- View OBD-II data
- Lock/unlock doors
- Remote start the engine
- Control dome lights

Consider adding authentication if deploying in a shared or public environment.

## GPIO Pinout (Alarm/Remote Start)

| GPIO | Function | Connection |
|------|----------|------------|
| 25 | Starter relay | Vehicle starter wire |
| 26 | Ignition relay | Vehicle ignition wire |
| 27 | Accessory relay | Vehicle accessory wire |
| 32 | Lock relay | Vehicle door lock wire |
| 33 | Unlock relay | Vehicle door unlock wire |
| 14 | Siren relay | Alarm siren/horn |
| 35 | Trigger input | Alarm trigger sensor |

**Relay wiring:** Active LOW (relay modules with opto-isolation recommended)

## License

This project is open source. Feel free to modify and distribute.

## Credits

- **PPP Implementation:** Based on [Zimodem](https://github.com/bozimmerman/Zimodem) by bozimmerman
- **NimBLE Library:** [h2zero/NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
- **ESP32 Platform:** [Espressif Systems](https://github.com/espressif/arduino-esp32)

## Support

For issues, questions, or contributions, please open an issue on GitHub.
