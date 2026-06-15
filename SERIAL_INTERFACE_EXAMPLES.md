# Serial Interface Usage Examples

## Overview
The ESP32 now responds to GET-style commands over the serial port (115200 baud).
Each command should end with a newline character (\n).

## Command Format
```
GET /api/endpoint
GET /api/endpoint?param1=value1&param2=value2
```

## Available Endpoints

### 1. Get Vehicle Data
```
Command:  GET /api/data
Response: {"speed":0.0,"rpm":0,"coolant":0.0,"fuel":0.0,"load":0.0,"voltage":12.5,"intake":0.0,"throttle":0.0,"economy":50.0,"mapMpg":0.0,"tripMiles":0.0,"mte":0,"checkEngine":false,"obdConnected":false}
```

### 2. Get Diagnostic Trouble Codes
```
Command:  GET /api/dtc
Response: {"codes":["P0133","P0420"],"connected":true}
```

### 3. Get OBD Status
```
Command:  GET /api/obd/status
Response: {"connected":true,"deviceName":"OBDII","deviceAddr":"AA:BB:CC:DD:EE:FF","wifiSSID":"ZCar_XXXXX","wifiPassword":"12345678","obdFilter":"OBD,ELM,V-LINK,IOS-","cylinders":6,"tankCapacity":16.0,"fixedMPG":23.0,"mapMPG":0.0,"devices":[...]}
```

### 4. Scan for BLE OBD Devices
```
Command:  GET /api/obd/scan
Response: [{"name":"OBDII","addr":"AA:BB:CC:DD:EE:FF","match":true},{"name":"ELM327","addr":"11:22:33:44:55:66","match":true}]
```

### 5. Connect to OBD Device
```
Command:  GET /api/obd/connect?addr=AA:BB:CC:DD:EE:FF
Response: {"success":true}
```

### 6. Disconnect from OBD Device
```
Command:  GET /api/obd/disconnect
Response: {"success":true}
```

### 7. Update Settings
```
Command:  GET /api/obd/settings?cylinders=6&tankCapacity=16.0&fixedMPG=25.0
Response: {"success":true}
```

### 8. Lock Doors
```
Command:  GET /api/control/lock
Response: {"success":true,"action":"lock"}
```

### 9. Unlock Doors
```
Command:  GET /api/control/unlock
Response: {"success":true,"action":"unlock"}
```

### 10. Dome Light Control
```
Command:  GET /api/control/domelight?state=1
Response: {"success":true,"state":true}

Command:  GET /api/control/domelight?state=0
Response: {"success":true,"state":false}
```

### 11. Remote Start
```
Command:  GET /api/control/remotestart
Response: {"success":true,"attempts":1}
Error:    {"success":false,"error":"OBD not connected"}
```

### 12. Get System Status
```
Command:  GET /api/control/status
Response: {"alarmArmed":false,"alarmTriggered":false,"engineRunning":false,"remoteStartActive":false,"obdConnected":true,"rpm":0}
```

### 13. Reset MPG Tracking
```
Command:  GET /api/mpg/reset
Response: {"success":true}
```

## Error Handling
```
Command:  GET /api/unknown
Response: {"error":"unknown route"}
```

## Python Example

```python
import serial
import time
import json

# Open serial connection
ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=1)
time.sleep(2)  # Wait for ESP32 to initialize

def send_command(cmd):
    ser.write(f"{cmd}\n".encode())
    response = ser.readline().decode().strip()
    return json.loads(response)

# Get vehicle data
data = send_command("GET /api/data")
print(f"Speed: {data['speed']} MPH")
print(f"RPM: {data['rpm']}")
print(f"Voltage: {data['voltage']} V")

# Lock doors
result = send_command("GET /api/control/lock")
print(f"Lock result: {result['success']}")

ser.close()
```

## Arduino Serial Monitor Example
1. Open Arduino Serial Monitor
2. Set baud rate to 115200
3. Set line ending to "Newline"
4. Type command: `GET /api/data`
5. Press Enter
6. View JSON response

## Benefits
- No WiFi required for control
- Direct USB connection to STM32 host
- Same API as WiFi interface
- Machine-readable JSON responses
- No debug output clutter
