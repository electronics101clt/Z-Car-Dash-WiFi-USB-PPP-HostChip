#include <WiFi.h>
#include <WebServer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "esp_coexist.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip4.h"

// Persistent settings
Preferences preferences;
uint32_t serialNumber;
char ssid[32];
char wifiPassword[32] = "12345678";
char obdFilter[64] = "OBD,ELM,V-LINK,IOS-";
int engineCylinders = 6;  // 4, 6, or 8
float engineDisplacement = 3.0;  // Inferred from cylinders
float tankCapacity = 16.0;  // Gallons (default: 2005 Chevy Malibu)
float fixedMPG = 23.0;  // Fixed MPG for miles till empty (default: 2005 Malibu combined)
float economyScore = 50;
float mapMPG = 0;
float milesTillEmpty = 0;
String connectedDeviceName = "";
String connectedDeviceAddr = "";
String foundDevicesJson = "[]";

// Actual MPG tracking (fill-to-fill average)
float totalMiles = 0;  // Miles since last fill
float lastFuelLevel = 0;
unsigned long lastMilesTime = 0;
float lastSavedMiles = 0;  // Track when to save to NVS
unsigned long stationaryStartTime = 0;  // Track when vehicle became stationary
bool wasStationary = false;  // Track stationary state
bool bootFillCheck = true;  // Check for fill on first loop after boot

// BLE ELM327 - common UUIDs
static NimBLEUUID serviceUUID("fff0");
static NimBLEUUID charTXUUID("fff1");  // Write to ELM
static NimBLEUUID charRXUUID("fff2");  // Read from ELM (notify)

NimBLEScan* pBLEScan;
NimBLEClient* pClient;
NimBLERemoteCharacteristic* pTXChar;
NimBLERemoteCharacteristic* pRXChar;
bool obdConnected = false;
unsigned long lastOBDQuery = 0;
int currentPID = 0;
String bleResponse = "";

// BLE disconnect callback
class ClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pClient) {
        obdConnected = false;
        pTXChar = nullptr;
        pRXChar = nullptr;
    }
};

// Web server on port 80
WebServer server(80);

// OBD-II data
float speed = 0;
float rpm = 0;
float coolantTemp = 0;
float fuelLevel = 0;
float engineLoad = 0;
float voltage = 0;
float intakeTemp = 0;
float throttle = 0;
float mapPressure = 0;
bool checkEngine = false;

// Alarm system GPIO pins
#define PIN_STARTER    25  // GPIO 25 - Starter relay
#define PIN_IGNITION   26  // GPIO 26 - Ignition relay
#define PIN_ACCESSORY  27  // GPIO 27 - Accessory relay
#define PIN_LOCK       32  // GPIO 32 - Lock relay
#define PIN_UNLOCK     33  // GPIO 33 - Unlock relay
#define PIN_SIREN      14  // GPIO 14 - Siren/Horn relay
#define PIN_TRIGGER    35  // GPIO 35 - Alarm trigger input

// Alarm timing constants
#define ECM_BOOT_WAIT        3000   // 3 seconds between ECM connection checks
#define STARTER_MAX_CRANK    5000   // 5 seconds max starter hold time
#define STARTER_POLL_INTERVAL 100   // Check RPM every 100ms during crank
#define RPM_RUNNING_THRESHOLD 500   // 500 RPM = engine running
#define RELAY_SETTLE_DELAY    100   // 100ms delay between relay activations

// Relay control (Active LOW for typical relay modules)
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// Alarm state variables
bool alarmArmed = false;
bool alarmTriggered = false;
bool sirenActive = false;
bool engineRunning = false;
bool remoteStartActive = false;
unsigned long lastTriggerCheck = 0;
unsigned long sirenStartTime = 0;
unsigned long remoteStartTimer = 0;
unsigned long tabletOnTimer = 0;
bool tabletOnTimerActive = false;
bool domeLightOn = false;

// Remote start constants
#define REMOTE_START_DURATION 600000  // 10 minutes (600,000 ms)
#define MAX_START_ATTEMPTS 3          // 3 start attempts before giving up
#define TABLET_ON_DURATION 300000     // 5 minutes (300,000 ms)

// Modem state variables
enum ModemState {
    MODEM_IDLE,
    MODEM_DIALING,
    MODEM_CONNECTED,
    MODEM_ONLINE
};

ModemState modemState = MODEM_IDLE;
String modemBuffer = "";
bool modemEcho = true;
bool modemVerbose = true;
bool modemConnected = false;

// Modem phonebook (stored in Preferences as JSON)
#define MAX_PHONEBOOK_ENTRIES 10
struct PhonebookEntry {
    String number;
    String ssid;
    String password;
};
PhonebookEntry phonebook[MAX_PHONEBOOK_ENTRIES];
int phonebookCount = 0;

// WiFi uplink credentials (for modem STA mode)
char staSSID[32] = "";
char staPassword[64] = "";
bool staConnected = false;

// Hayes escape sequence (+++  with guard times)
#define ESC_GUARD_TIME 1000  // 1 second guard time in milliseconds
#define ESC_CHARACTER '+'    // Escape character
#define ESC_TIMES 3          // Number of escape chars needed
unsigned long escCheckTime = 0;  // Time tracking for guard period
byte escCounter = 0;             // Count of escape chars seen
unsigned long lastSerialActivity = 0;  // Last time serial data received

// PPP Protocol Constants
#define PPP_FLAG     0x7E
#define PPP_ESCAPE   0x7D
#define PPP_TRANS    0x20

#define PPP_PROTOCOL_LCP    0xC021
#define PPP_PROTOCOL_IPCP   0x8021
#define PPP_PROTOCOL_IP     0x0021

#define LCP_CONF_REQ    1
#define LCP_CONF_ACK    2
#define LCP_CONF_NAK    3
#define LCP_CONF_REJ    4
#define LCP_TERM_REQ    5
#define LCP_TERM_ACK    6
#define LCP_CODE_REJ    7
#define LCP_ECHO_REQ    8
#define LCP_ECHO_REPLY  9

enum PPPState {
    PPP_DEAD,
    PPP_ESTABLISH,
    PPP_OPENED,
    PPP_TERMINATE
};

// PPP state variables
uint8_t *pppBuf = NULL;
int pppMaxBufSize = 0;
int pppCurBufLen = 0;
bool pppEscaped = false;
PPPState pppState = PPP_DEAD;
uint8_t lcpId = 0;
uint8_t ipcpId = 0;
bool lcpOpened = false;
bool ipcpOpened = false;

struct netif ppp_netif;
struct netif *wifi_netif = NULL;
netif_output_fn original_wifi_output = NULL;
netif_input_fn original_wifi_input = NULL;

// Forward declarations
String getDTCType(char c);
String getQueryParam(String params, String key);
void ppp_start();
void ppp_shutdown();
void ppp_serialIncoming();
void ppp_sendPacketToSerial(struct pbuf *p);

// HTML dashboard length (measured, not strlen - base64 data contains null bytes)
#define HTML_LENGTH 32299

// Professional car dashboard with animated needle gauges
const char* html = R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Car Dashboard</title><style>*{margin:0;padding:0;box-sizing:border-box}body{background:linear-gradient(to bottom,#4a90e2,#1a3a52);color:#fff;font-family:Arial,sans-serif;overflow-x:hidden;overflow-y:auto}.dash{display:flex;flex-wrap:wrap;gap:12px;padding:12px;justify-content:center;align-items:center}.gauge{position:relative}.gauge canvas{display:block}.gauge-label{position:absolute;top:10px;left:50%;transform:translateX(-50%);font-size:11px;font-weight:bold;letter-spacing:1px;color:#fff;text-shadow:0 0 5px rgba(0,0,0,0.8)}.gauge-value{position:absolute;bottom:25%;left:50%;transform:translateX(-50%);font-size:24px;font-weight:bold;color:#fff;text-shadow:0 0 10px rgba(255,255,255,0.5)}.gauge-unit{position:absolute;bottom:20%;left:50%;transform:translateX(-50%);font-size:12px;color:#fff}.mpg-bar-container{width:90%;max-width:800px;margin:8px auto;padding:8px 20px;background:linear-gradient(to bottom,#0f1f3d,#060d1a);border-radius:8px}.mpg-label{font-size:12px;font-weight:bold;color:#fff;margin-bottom:5px;letter-spacing:1px}.mpg-bar{height:30px;background:linear-gradient(to bottom,#0f1f3d,#060d1a),url('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAAGYktHRAAAAAAAAPlDu38AAAAJcEhZcwAADsMAAA7DAcdvqGQAAAAHdElNRQfqBgoGHQUQfWOhAAAFuElEQVRIx1VWS44ltxGMSLJa3TOWdQ95JWhhC9AhDG99aAmQbmEJGM3Mq+IvI7xgdWtEFN5jgQUmGRkZkfzvf/495qy1llJgSy4lnp9fvv/+e8Pn47jadZ3n2dqn83H1dp1nu652tus6W2ufH9cfHz9e5/X1N38nEWAwDBug8XQctfcx51BmKSHJQIlSSgFAgASxp4ggwRIRYJDBIGMvJtxHDxJGiZCNe7j23uccY46jFlsAyVJrxV+HYYCwbb+FvV/BtNocNAgEA7YBEEbW1q61MoK5QneAOI4nSRE0LC1pQXYKgA3bstNKCXYABGYftoP72vtAgFVb65JKibloJRCAn5+fyY0JQN4ntgkYMAkS5L0LQCNXwtAGlHuBJV17b2stRpQo95oxel9r2UX7wIYkAJbvK9wo06QB2fJfAN1/maptjFwrGFHibWmMacDeV6XvbWUJkrVHpnL/WrqZYPhtf0BWbXNorijBpLQB4NXbmDMiV6Z13zrIo0Z9/66WEowAaRK0pMwx1+vdrP1IAdQ2es5VGEEKMEzy46ePv/z6i1IlIhhrDcMEnl9e/vHttzPzcbbzPK/zPK/rOs/zOlsbV+/XdY0xWu+P8/z8+XO7rtpad2ZhkEW2IRKV8eHDB0lHPWpEagAopRxPx/v371eKUYIsEWWPWo6jl6OWUq7rRGCuERE26uhTuYLBiExF3Gic50lSK7OU1LQVEU9Pz35DwbbkV9pqA6OUlGspBRtkvVq3kiRIyQwUgOQYg2Ry1VLlZZvky8vI1Bd5v9kFGtYOBAMCDAJ21syUjTAMG0wkUTOv3oMsYCkFlq2IMsYEowQYKMGIiAiS5P1SSo0yGYjwLqLqTXV9wWBjSe01wFGKoF2lvQ2nRUsrc+XrUCrTmZJspbSk3MStNwnNLxlsoY8ZRiGzFillBWP0bqUJaWchIWFTMlNK54IMEQJkGfX5q6ddlb7z5ZQIjDEoB1FXSSdgIvocc04GLW9gSimlxHEc74MRLGQh74ukp1x//PGHMUZvvbWr935e7ePnxxpj9gEpgjMoiwTND398+PnXX+wMIEqZc8I08O7l5btvvxtztKtd5/X5fFztejzOx3nWb77+urVWyaAJr7WOEsseY0AmwaCsIG2U0v73+++FqCVqKZJklChPx/H+/bvndRQGAdNRgiQj6q1fAEH7lmNJfQ6nNkmkJGmjHk9jzIBXsJSAITkivnr+SkpJ0s68JG3M620dt3nYJkzZvXdJZGybIUj76emp90a4MGoJm6ksES/95U25yU1cbpmvIEy8etyt/7bnSsGEttLRADDm7H0QLuQqZRdaRIwxDfLO8fYOwwii7jgM8rVogFud444ICyYsrFTrI2DC21adAjHmILZjwnYABRFgYVTPvHHLtdbKnJv1X1oHDb2qwdVaJTYdDFMmOfqQlG/wS2+T+u75OV5NXLIy11p9zjnXa262mu26cR9n4hYH2wSC3J1DSmutV3Pzlo/6wz//dY3+OM/H+bjO63E+zuv69Dh773PM1lobo139cZ6fH5/n7HPO3OwN2qYRwY+fPv3080+WSJZyrLWkJFGPo777+m9x1Z3zwigRtdZSa2+tt1b2SVNr1l7KHBhjbk2JCBiwgyz18dvvv9GutZZySAYUwefnl/rKzj/d/BZ8eBuclFvoDUnqfWy5ji2YNsljHKNPOOfM48itnwyQUbmp4luu9+luqsHg2/ye9DFg02CE9idErbX1QSsiV87deBEM1rr7NeCuj3jtCO/WByjBQuy+0/DKtE0T2o5jAONYrfeASwRXwJuxLOWoW283923rdkHAhrY5StLWj90uQG/N5K0DssackLYLbXYBrKXWV+dzprbJppW4PTaltNO6txNp6gb0z0ZLUBvX1q4SBdLGs9ZS77bgbppRXl1w99j17WEEGW/1+iYsgAFJYwzLEVG43vq+43j6P7S1uJesljbPAAAAAElFTkSuQmCC');background-blend-mode:overlay;background-size:32px 32px;border-radius:15px;overflow:hidden;border:1px solid #333;position:relative}.mpg-bar::after{content:'';position:absolute;left:50%;top:0;width:2px;height:100%;background:#fff;transform:translateX(-50%)}.mpg-bar-no-mid{height:30px;background:linear-gradient(to bottom,#0f1f3d,#060d1a),url('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAAGYktHRAAAAAAAAPlDu38AAAAJcEhZcwAADsMAAA7DAcdvqGQAAAAHdElNRQfqBgoGHQUQfWOhAAAFuElEQVRIx1VWS44ltxGMSLJa3TOWdQ95JWhhC9AhDG99aAmQbmEJGM3Mq+IvI7xgdWtEFN5jgQUmGRkZkfzvf/495qy1llJgSy4lnp9fvv/+e8Pn47jadZ3n2dqn83H1dp1nu652tus6W2ufH9cfHz9e5/X1N38nEWAwDBug8XQctfcx51BmKSHJQIlSSgFAgASxp4ggwRIRYJDBIGMvJtxHDxJGiZCNe7j23uccY46jFlsAyVJrxV+HYYCwbb+FvV/BtNocNAgEA7YBEEbW1q61MoK5QneAOI4nSRE0LC1pQXYKgA3bstNKCXYABGYftoP72vtAgFVb65JKibloJRCAn5+fyY0JQN4ntgkYMAkS5L0LQCNXwtAGlHuBJV17b2stRpQo95oxel9r2UX7wIYkAJbvK9wo06QB2fJfAN1/maptjFwrGFHibWmMacDeV6XvbWUJkrVHpnL/WrqZYPhtf0BWbXNorijBpLQB4NXbmDMiV6Z13zrIo0Z9/66WEowAaRK0pMwx1+vdrP1IAdQ2es5VGEEKMEzy46ePv/z6i1IlIhhrDcMEnl9e/vHttzPzcbbzPK/zPK/rOs/zOlsbV+/XdY0xWu+P8/z8+XO7rtpad2ZhkEW2IRKV8eHDB0lHPWpEagAopRxPx/v371eKUYIsEWWPWo6jl6OWUq7rRGCuERE26uhTuYLBiExF3Gic50lSK7OU1LQVEU9Pz35DwbbkV9pqA6OUlGspBRtkvVq3kiRIyQwUgOQYg2Ry1VLlZZvky8vI1Bd5v9kFGtYOBAMCDAJ21syUjTAMG0wkUTOv3oMsYCkFlq2IMsYEowQYKMGIiAiS5P1SSo0yGYjwLqLqTXV9wWBjSe01wFGKoF2lvQ2nRUsrc+XrUCrTmZJspbSk3MStNwnNLxlsoY8ZRiGzFillBWP0bqUJaWchIWFTMlNK54IMEQJkGfX5q6ddlb7z5ZQIjDEoB1FXSSdgIvocc04GLW9gSimlxHEc74MRLGQh74ukp1x//PGHMUZvvbWr935e7ePnxxpj9gEpgjMoiwTND398+PnXX+wMIEqZc8I08O7l5btvvxtztKtd5/X5fFztejzOx3nWb77+urVWyaAJr7WOEsseY0AmwaCsIG2U0v73+++FqCVqKZJklChPx/H+/bvndRQGAdNRgiQj6q1fAEH7lmNJfQ6nNkmkJGmjHk9jzIBXsJSAITkivnr+SkpJ0s68JG3M620dt3nYJkzZvXdJZGybIUj76emp90a4MGoJm6ksES/95U25yU1cbpmvIEy8etyt/7bnSsGEttLRADDm7H0QLuQqZRdaRIwxDfLO8fYOwwii7jgM8rVogFud444ICyYsrFTrI2DC21adAjHmILZjwnYABRFgYVTPvHHLtdbKnJv1X1oHDb2qwdVaJTYdDFMmOfqQlG/wS2+T+u75OV5NXLIy11p9zjnXa262mu26cR9n4hYH2wSC3J1DSmutV3Pzlo/6wz//dY3+OM/H+bjO63E+zuv69Dh773PM1lobo139cZ6fH5/n7HPO3OwN2qYRwY+fPv3080+WSJZyrLWkJFGPo777+m9x1Z3zwigRtdZSa2+tt1b2SVNr1l7KHBhjbk2JCBiwgyz18dvvv9GutZZySAYUwefnl/rKzj/d/BZ8eBuclFvoDUnqfWy5ji2YNsljHKNPOOfM48itnwyQUbmp4luu9+luqsHg2/ye9DFg02CE9idErbX1QSsiV87deBEM1rr7NeCuj3jtCO/WByjBQuy+0/DKtE0T2o5jAONYrfeASwRXwJuxLOWoW283923rdkHAhrY5StLWj90uQG/N5K0DssackLYLbXYBrKXWV+dzprbJppW4PTaltNO6txNp6gb0z0ZLUBvX1q4SBdLGs9ZS77bgbppRXl1w99j17WEEGW/1+iYsgAFJYwzLEVG43vq+43j6P7S1uJesljbPAAAAAElFTkSuQmCC');background-blend-mode:overlay;background-size:32px 32px;border-radius:15px;overflow:hidden;border:1px solid #333;position:relative}.mpg-fill{position:absolute;height:100%;width:0%;left:50%;background:#0f6;transition:width 0.3s,left 0.3s}.mpg-value{font-size:18px;font-weight:bold;color:#fff;text-align:center;margin-top:5px}.warnings{display:flex;gap:10px;justify-content:center;padding:6px;position:fixed;bottom:0;width:100%;background:rgba(17,17,17,0.95);backdrop-filter:blur(10px);border-top:1px solid #333}.warn{padding:6px 12px;border-radius:4px;font-size:12px;font-weight:bold;background:#333;color:#fff;border:2px solid #555;transition:all 0.3s;cursor:pointer}.warn.on{background:#d00;color:#fff;border-color:#f44;box-shadow:0 0 20px rgba(255,68,68,0.6);animation:pulse 1s infinite}.modal{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.9);z-index:1000;justify-content:center;align-items:center;overflow-y:auto}.modal-content{background:#222;padding:30px;border-radius:10px;max-width:500px;width:90%;max-height:90vh;overflow-y:auto;border:2px solid #f44;box-shadow:0 0 30px rgba(255,68,68,0.5);margin:20px 0}.modal-title{font-size:20px;font-weight:bold;color:#f44;margin-bottom:20px;text-align:center}.modal-close{position:absolute;top:15px;right:20px;font-size:30px;color:#888;cursor:pointer;transition:color 0.3s}.modal-close:hover{color:#fff}.dtc-list{list-style:none;max-height:300px;overflow-y:auto}.dtc-item{padding:10px;margin:8px 0;background:#333;border-left:4px solid #f44;border-radius:4px;font-family:monospace;font-size:14px}.dtc-empty{text-align:center;color:#888;padding:20px;font-size:14px}.obd-section{margin:15px 0;padding:10px;background:#333;border-radius:6px}.obd-section h4{margin:0 0 10px;color:#4cf;font-size:12px;text-transform:uppercase}.obd-row{display:flex;justify-content:space-between;align-items:center;margin:8px 0}.obd-label{color:#888;font-size:12px}.obd-value{color:#fff;font-size:13px;font-family:monospace}.obd-input{background:#222;border:1px solid #555;color:#fff;padding:6px 10px;border-radius:4px;width:180px;font-size:13px}.obd-btn{padding:6px 12px;border:none;border-radius:4px;font-size:11px;font-weight:bold;cursor:pointer;margin:2px}.obd-btn-blue{background:#28f;color:#fff}.obd-btn-blue:hover{background:#39f}.obd-btn-green{background:#2a2;color:#fff}.obd-btn-green:hover{background:#3b3}.obd-btn-red{background:#a22;color:#fff}.obd-btn-red:hover{background:#c33}.obd-btn-gray{background:#555;color:#fff}.obd-btn-gray:hover{background:#666}.device-list{max-height:150px;overflow-y:auto}.device-item{display:flex;justify-content:space-between;align-items:center;padding:8px;margin:4px 0;background:#222;border-radius:4px;font-size:12px}.device-name{color:#fff}.device-addr{color:#666;font-family:monospace;font-size:10px}.device-match{border-left:3px solid #2a2}.device-nomatch{border-left:3px solid #555}@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.7}}@media(max-width:768px){.gauge canvas{width:120px!important;height:120px!important}.gauge-value{font-size:18px}.drawer-content{grid-template-columns:1fr;padding:20px}}@media(orientation:portrait){.dash{flex-direction:column}.gauge canvas{width:180px!important;height:180px!important}.mpg-bar-container{max-width:90%}.drawer-content{grid-template-columns:1fr;max-width:100%}}@media(min-width:769px){.drawer-content{grid-template-columns:repeat(2,1fr)}}.controls{position:fixed;right:10px;bottom:70px;display:flex;flex-direction:column;align-items:center;justify-content:center;background:rgba(17,17,17,0.95);backdrop-filter:blur(10px);border:1px solid #333;border-radius:8px;padding:15px;z-index:1001;writing-mode:vertical-rl;text-orientation:upright;font-size:18px;font-weight:bold;color:#fff;letter-spacing:5px;cursor:pointer;transition:right 0.3s ease}.controls:hover{background:rgba(34,34,34,0.95)}.controls.drawer-open{right:calc(100% - 60px)}.drawer{position:fixed;top:0;right:-100%;width:100%;height:100%;background:rgba(10,26,42,0.85);backdrop-filter:blur(10px);z-index:1000;transition:right 0.3s ease;overflow-y:auto;padding:20px;box-sizing:border-box}.drawer.open{right:0}.drawer-content{max-width:900px;margin:0 auto;padding:40px 20px;display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:20px;align-content:center;min-height:100vh}.drawer-section{background:rgba(17,17,17,0.8);border-radius:8px;padding:20px;margin:15px 0;border:1px solid #333}.control-btn{background:linear-gradient(to bottom,#0f1f3d,#060d1a),url('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAAGYktHRAAAAAAAAPlDu38AAAAJcEhZcwAADsMAAA7DAcdvqGQAAAAHdElNRQfqBgoGHQUQfWOhAAAFuElEQVRIx1VWS44ltxGMSLJa3TOWdQ95JWhhC9AhDG99aAmQbmEJGM3Mq+IvI7xgdWtEFN5jgQUmGRkZkfzvf/495qy1llJgSy4lnp9fvv/+e8Pn47jadZ3n2dqn83H1dp1nu652tus6W2ufH9cfHz9e5/X1N38nEWAwDBug8XQctfcx51BmKSHJQIlSSgFAgASxp4ggwRIRYJDBIGMvJtxHDxJGiZCNe7j23uccY46jFlsAyVJrxV+HYYCwbb+FvV/BtNocNAgEA7YBEEbW1q61MoK5QneAOI4nSRE0LC1pQXYKgA3bstNKCXYABGYftoP72vtAgFVb65JKibloJRCAn5+fyY0JQN4ntgkYMAkS5L0LQCNXwtAGlHuBJV17b2stRpQo95oxel9r2UX7wIYkAJbvK9wo06QB2fJfAN1/maptjFwrGFHibWmMacDeV6XvbWUJkrVHpnL/WrqZYPhtf0BWbXNorijBpLQB4NXbmDMiV6Z13zrIo0Z9/66WEowAaRK0pMwx1+vdrP1IAdQ2es5VGEEKMEzy46ePv/z6i1IlIhhrDcMEnl9e/vHttzPzcbbzPK/zPK/rOs/zOlsbV+/XdY0xWu+P8/z8+XO7rtpad2ZhkEW2IRKV8eHDB0lHPWpEagAopRxPx/v371eKUYIsEWWPWo6jl6OWUq7rRGCuERE26uhTuYLBiExF3Gic50lSK7OU1LQVEU9Pz35DwbbkV9pqA6OUlGspBRtkvVq3kiRIyQwUgOQYg2Ry1VLlZZvky8vI1Bd5v9kFGtYOBAMCDAJ21syUjTAMG0wkUTOv3oMsYCkFlq2IMsYEowQYKMGIiAiS5P1SSo0yGYjwLqLqTXV9wWBjSe01wFGKoF2lvQ2nRUsrc+XrUCrTmZJspbSk3MStNwnNLxlsoY8ZRiGzFillBWP0bqUJaWchIWFTMlNK54IMEQJkGfX5q6ddlb7z5ZQIjDEoB1FXSSdgIvocc04GLW9gSimlxHEc74MRLGQh74ukp1x//PGHMUZvvbWr935e7ePnxxpj9gEpgjMoiwTND398+PnXX+wMIEqZc8I08O7l5btvvxtztKtd5/X5fFztejzOx3nWb77+urVWyaAJr7WOEsseY0AmwaCsIG2U0v73+++FqCVqKZJklChPx/H+/bvndRQGAdNRgiQj6q1fAEH7lmNJfQ6nNkmkJGmjHk9jzIBXsJSAITkivnr+SkpJ0s68JG3M620dt3nYJkzZvXdJZGybIUj76emp90a4MGoJm6ksES/95U25yU1cbpmvIEy8etyt/7bnSsGEttLRADDm7H0QLuQqZRdaRIwxDfLO8fYOwwii7jgM8rVogFud444ICyYsrFTrI2DC21adAjHmILZjwnYABRFgYVTPvHHLtdbKnJv1X1oHDb2qwdVaJTYdDFMmOfqQlG/wS2+T+u75OV5NXLIy11p9zjnXa262mu26cR9n4hYH2wSC3J1DSmutV3Pzlo/6wz//dY3+OM/H+bjO63E+zuv69Dh773PM1lobo139cZ6fH5/n7HPO3OwN2qYRwY+fPv3080+WSJZyrLWkJFGPo777+m9x1Z3zwigRtdZSa2+tt1b2SVNr1l7KHBhjbk2JCBiwgyz18dvvv9GutZZySAYUwefnl/rKzj/d/BZ8eBuclFvoDUnqfWy5ji2YNsljHKNPOOfM48itnwyQUbmp4luu9+luqsHg2/ye9DFg02CE9idErbX1QSsiV87deBEM1rr7NeCuj3jtCO/WByjBQuy+0/DKtE0T2o5jAONYrfeASwRXwJuxLOWoW283923rdkHAhrY5StLWj90uQG/N5K0DssackLYLbXYBrKXWV+dzprbJppW4PTaltNO6txNp6gb0z0ZLUBvX1q4SBdLGs9ZS77bgbppRXl1w99j17WEEGW/1+iYsgAFJYwzLEVG43vq+43j6P7S1uJesljbPAAAAAElFTkSuQmCC');background-blend-mode:overlay;background-size:32px 32px;border:2px solid #333;border-radius:8px;padding:30px 20px;font-size:16px;font-weight:bold;color:#fff;cursor:pointer;transition:all 0.3s;box-shadow:0 0 15px rgba(0,0,0,0.5);width:100%;min-height:120px;display:flex;align-items:center;justify-content:center;margin:10px 0}.control-btn:hover{box-shadow:0 0 25px rgba(68,204,255,0.5);border-color:#4cf}.control-btn:active{transform:scale(0.98)}.drawer-title{font-size:24px;font-weight:bold;color:#4cf;margin-bottom:20px;text-align:center}.mpg-bar-container::after,.warnings::after,.controls::after{content:'';position:absolute;top:0;left:0;right:0;bottom:0;background:linear-gradient(135deg,rgba(255,255,255,0.2) 0%,transparent 50%,rgba(0,0,0,0.1) 100%);border-radius:inherit;pointer-events:none}.mpg-bar-container,.warnings{position:relative}</style></head><body><div class="dash"><div class="gauge"><canvas id="g1" width="260" height="260"></canvas><div class="gauge-value" id="v1">0</div><div class="gauge-unit">MPH</div></div><div class="gauge"><canvas id="g2" width="260" height="260"></canvas><div class="gauge-value" id="v2">0</div><div class="gauge-unit">RPM</div></div><div class="gauge"><canvas id="g3" width="200" height="200"></canvas><div class="gauge-value" id="v3">0</div><div class="gauge-unit">°F</div></div><div class="gauge"><canvas id="g4" width="200" height="200"></canvas><div class="gauge-value" id="v4">0</div><div class="gauge-unit">V</div></div></div><div class="mpg-bar-container"><div class="mpg-label">ECONOMY</div><div class="mpg-bar"><div class="mpg-fill" id="mpgFill"></div></div></div><div class="mpg-bar-container"><div class="mpg-label">MPG</div><div class="mpg-bar-no-mid"><div class="mpg-fill" id="mapMpgFill"></div><div class="mpg-value" style="position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);font-size:16px;font-weight:bold;color:#fff;text-shadow:0 0 5px rgba(0,0,0,0.8);z-index:10"><span id="mapMpgVal">0</span> MPG</div></div></div><div class="mpg-bar-container"><div class="mpg-label">MILES TILL EMPTY</div><div class="mpg-bar-no-mid"><div class="mpg-fill" id="mteFill"></div><div class="mpg-value" style="position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);font-size:16px;font-weight:bold;color:#fff;text-shadow:0 0 5px rgba(0,0,0,0.8);z-index:10"><span id="mteVal">0</span> MI</div></div></div><div class="drawer" id="drawer"><div class="drawer-content"><div style="grid-column:1/-1;background:rgba(17,17,17,0.9);border-radius:12px;padding:20px;margin-bottom:20px;border:1px solid #333"><div style="font-size:18px;font-weight:bold;color:#4cf;margin-bottom:15px;text-align:center">Status</div><div id="statusDisplay" style="display:grid;grid-template-columns:1fr 1fr;gap:10px;font-size:16px"><div style="padding:12px;text-align:center"><span style="color:#fff">🚨 Alarm:</span><span id="statAlarm" style="color:#888">DISARMED</span></div><div style="padding:12px;text-align:center"><span style="color:#fff">Engine:</span><span id="statEngine" style="color:#888">OFFLINE</span></div></div></div><button class="control-btn" onclick="sendLock()">🔒 LOCK</button><button class="control-btn" onclick="sendUnlock()">🔓 UNLOCK</button><button class="control-btn" id="domeLightBtn" onclick="toggleDomeLight()">💡 DOME LIGHT</button><button class="control-btn" onclick="sendRemoteStart()">🚗 REMOTE START</button><hr style="border:none;border-top:1px solid rgba(255,255,255,0.2);margin:20px 0"></div></div><div class="controls" id="controlsBtn" onclick="toggleDrawer()">CONTROLS</div><div class="warnings"><span class="warn" id="ce" onclick="showDTC()">CHECK ENGINE</span><span class="warn" id="st" onclick="showOBD()">SETTINGS</span><span style="display:flex;align-items:center;gap:4px;font-size:14px;font-weight:bold;color:#fff;margin-left:auto;padding-right:6px;"><span id="obdDot" style="width:12px;height:12px;border-radius:50%;background:#f55;display:inline-block;"></span>OBD</span></div><div class="modal" id="dtcModal" onclick="if(event.target==this)closeDTC()"><div class="modal-content"><div class="modal-title">DIAGNOSTIC TROUBLE CODES</div><ul class="dtc-list" id="dtcList"><li class="dtc-empty">Loading...</li></ul></div></div><div class="modal" id="obdModal" onclick="if(event.target==this)closeOBD()"><div class="modal-content" style="border-color:#4cf;box-shadow:0 0 30px rgba(68,204,255,0.5)"><div class="modal-title" style="color:#4cf">Settings</div><div class="obd-section"><h4>Connection Status</h4><div class="obd-row"><span class="obd-label">Status</span><span class="obd-value" id="obdStatus">--</span></div><div class="obd-row"><span class="obd-label">Device</span><span class="obd-value" id="obdDevice">--</span></div><div class="obd-row"><span class="obd-label">Address</span><span class="obd-value" id="obdAddr">--</span></div></div><div class="obd-section"><h4>Settings</h4><div class="obd-row"><span class="obd-label">WiFi Password</span><input type="text" class="obd-input" id="obdWifiPass" value="" onchange="obdSave()"></div><div class="obd-row"><span class="obd-label">Device Filter</span><input type="text" class="obd-input" id="obdFilterInput" value="" onchange="obdSave()"></div></div><div class="obd-section"><h4>MPG Tracking</h4><div class="obd-row"><span class="obd-label">Cylinders</span><select class="obd-input" id="obdCylinders" onchange="obdSave()" style="width:80px"><option value="4">4 cyl</option><option value="6">6 cyl</option><option value="8">8 cyl</option></select></div><div class="obd-row"><span class="obd-label">Tank Capacity</span><input type="number" class="obd-input" id="obdTankCap" value="16.0" min="5" max="40" step="0.5" onchange="obdSave()" style="width:80px"> gal</div><div class="obd-row"><span class="obd-label">MPG Setting</span><input type="number" class="obd-input" id="obdFixedMPG" value="23.0" min="5" max="60" step="0.5" onchange="obdSave()" style="width:80px"> MPG</div><div class="obd-row"><span class="obd-label">Refined Average</span><span class="obd-value" id="obdRefinedMPG">--</span></div><div class="obd-row"><button onclick="resetMPG()" style="padding:8px 16px;background:#f55;color:#fff;border:none;border-radius:4px;cursor:pointer;font-weight:bold;">RESET MPG DATA</button></div></div><div class="obd-section"><h4>Available Devices</h4><div class="device-list" id="deviceList"><div class="dtc-empty">Click Scan to find devices</div></div></div><div style="display:flex;gap:10px;justify-content:center;margin-top:15px"><button class="obd-btn obd-btn-blue" onclick="obdScan()">Scan</button></div></div></div><script>function drawGauge(ctx,w,h,val,min,max,zones){ctx.clearRect(0,0,w,h);const cx=w/2,cy=h/2,r=Math.min(w,h)/2-15;const sa=-0.75*Math.PI,ea=0.75*Math.PI,range=ea-sa;ctx.save();ctx.translate(cx,cy);const grad=ctx.createRadialGradient(0,0,r*0.5,0,0,r);grad.addColorStop(0,'#0f1f3d');grad.addColorStop(1,'#060d1a');ctx.fillStyle=grad;ctx.beginPath();ctx.arc(0,0,r,0,2*Math.PI);ctx.fill();ctx.strokeStyle='#333';ctx.lineWidth=2;ctx.stroke();if(fiberImg.complete&&fiberImg.naturalWidth>0){const pat=ctx.createPattern(fiberImg,'repeat');ctx.save();ctx.globalCompositeOperation='lighten';ctx.globalAlpha=0.5;ctx.fillStyle=pat;ctx.beginPath();ctx.arc(0,0,r,0,2*Math.PI);ctx.fill();ctx.restore();}const ticks=20;for(let i=0;i<=ticks;i++){const a=sa+range*i/ticks;const isMajor=i%5===0;ctx.save();ctx.rotate(a);ctx.strokeStyle='#555';ctx.lineWidth=isMajor?2:1;ctx.beginPath();ctx.moveTo(r-10,0);ctx.lineTo(r-(isMajor?20:12),0);ctx.stroke();if(isMajor){ctx.fillStyle='#fff';ctx.font='10px Arial';ctx.textAlign='center';ctx.textBaseline='middle';const num=Math.round(min+(max-min)*i/ticks);ctx.fillText(num,r-30,0);}ctx.restore();}const p=(val-min)/(max-min);const aa=sa+range*Math.max(0,Math.min(1,p));zones.forEach(z=>{if(p>=z.start&&p<z.end){ctx.strokeStyle=z.color;ctx.lineWidth=8;ctx.beginPath();ctx.arc(0,0,r-5,sa+range*z.start,sa+range*z.end);ctx.stroke();}});ctx.save();ctx.rotate(aa);const grd=ctx.createLinearGradient(0,0,r-25,0);grd.addColorStop(0,'#444');grd.addColorStop(1,'#fff');ctx.fillStyle=grd;ctx.beginPath();ctx.moveTo(-8,0);ctx.lineTo(r-25,0);ctx.lineTo(r-20,3);ctx.lineTo(-5,3);ctx.closePath();ctx.fill();ctx.strokeStyle='#fff';ctx.lineWidth=1;ctx.stroke();ctx.restore();ctx.fillStyle='#888';ctx.beginPath();ctx.arc(0,0,12,0,2*Math.PI);ctx.fill();ctx.fillStyle='#fff';ctx.beginPath();ctx.arc(0,0,8,0,2*Math.PI);ctx.fill();ctx.restore();}const fiberImg=new Image();const gauges=[{id:'g1',v:'v1',min:0,max:140,zones:[{start:0,end:0.7,color:'#0f6'},{start:0.7,end:0.9,color:'#ff0'},{start:0.9,end:1,color:'#f55'}]},{id:'g2',v:'v2',min:0,max:8000,zones:[{start:0,end:0.7,color:'#0f6'},{start:0.7,end:0.85,color:'#ff0'},{start:0.85,end:1,color:'#f55'}]},{id:'g3',v:'v3',min:120,max:270,zones:[{start:0,end:0.6,color:'#4cf'},{start:0.6,end:0.85,color:'#0f6'},{start:0.85,end:1,color:'#f55'}]},{id:'g4',v:'v4',min:10,max:16,zones:[{start:0,end:0.3,color:'#f55'},{start:0.3,end:0.6,color:'#ff0'},{start:0.6,end:1,color:'#0f6'}]}].map(g=>({...g,canvas:document.getElementById(g.id),ctx:document.getElementById(g.id).getContext('2d'),val:0}));let ecoSmooth=50,mapMpgSmooth=0;async function update(){try{const r=await fetch('/api/data');const d=await r.json();const vals=[d.speed*0.621371,d.rpm,d.coolant*9/5+32,d.voltage];gauges.forEach((g,i)=>{g.val=g.val*0.8+vals[i]*0.2;drawGauge(g.ctx,g.canvas.width,g.canvas.height,g.val,g.min,g.max,g.zones);if(i===3)document.getElementById(g.v).textContent=g.val.toFixed(1);else document.getElementById(g.v).textContent=Math.round(g.val);});ecoSmooth=ecoSmooth*0.8+d.economy*0.2;const fill=document.getElementById('mpgFill');const mid=50;if(ecoSmooth>=mid){fill.style.left='50%';fill.style.width=((ecoSmooth-mid)/mid*50)+'%';fill.style.background='#0f6';}else{const w=(mid-ecoSmooth)/mid*50;fill.style.left=(50-w)+'%';fill.style.width=w+'%';fill.style.background='#f55';}mapMpgSmooth=mapMpgSmooth*0.8+d.mapMpg*0.2;document.getElementById('mapMpgVal').textContent=mapMpgSmooth.toFixed(1)+' MPG | '+d.tripMiles.toFixed(1)+' mi';const mapFill=document.getElementById('mapMpgFill');const mpgPercent=Math.min(mapMpgSmooth/50*100,100);mapFill.style.left='0';mapFill.style.width=mpgPercent+'%';if(mapMpgSmooth>30)mapFill.style.background='#0f6';else if(mapMpgSmooth>15)mapFill.style.background='#ff0';else mapFill.style.background='#f55';document.getElementById('mteVal').textContent=Math.round(d.mte);const mteFill=document.getElementById('mteFill');const fuelPercent=Math.min(d.fuel,100);mteFill.style.left='0';mteFill.style.width=fuelPercent+'%';mteFill.style.background='#0f6';document.getElementById('ce').className=d.checkEngine?'warn on':'warn';document.getElementById('st').className='warn';document.getElementById('obdDot').style.background=d.obdConnected?'#0f6':'#f55';}catch(e){}}fiberImg.onload=function(){gauges.forEach(g=>drawGauge(g.ctx,g.canvas.width,g.canvas.height,0,g.min,g.max,g.zones));update();setInterval(update,100);};fiberImg.src='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAAGYktHRAAAAAAAAPlDu38AAAAJcEhZcwAADsMAAA7DAcdvqGQAAAAHdElNRQfqBgoGHQUQfWOhAAAFuElEQVRIx1VWS44ltxGMSLJa3TOWdQ95JWhhC9AhDG99aAmQbmEJGM3Mq+IvI7xgdWtEFN5jgQUmGRkZkfzvf/495qy1llJgSy4lnp9fvv/+e8Pn47jadZ3n2dqn83H1dp1nu652tus6W2ufH9cfHz9e5/X1N38nEWAwDBug8XQctfcx51BmKSHJQIlSSgFAgASxp4ggwRIRYJDBIGMvJtxHDxJGiZCNe7j23uccY46jFlsAyVJrxV+HYYCwbb+FvV/BtNocNAgEA7YBEEbW1q61MoK5QneAOI4nSRE0LC1pQXYKgA3bstNKCXYABGYftoP72vtAgFVb65JKibloJRCAn5+fyY0JQN4ntgkYMAkS5L0LQCNXwtAGlHuBJV17b2stRpQo95oxel9r2UX7wIYkAJbvK9wo06QB2fJfAN1/maptjFwrGFHibWmMacDeV6XvbWUJkrVHpnL/WrqZYPhtf0BWbXNorijBpLQB4NXbmDMiV6Z13zrIo0Z9/66WEowAaRK0pMwx1+vdrP1IAdQ2es5VGEEKMEzy46ePv/z6i1IlIhhrDcMEnl9e/vHttzPzcbbzPK/zPK/rOs/zOlsbV+/XdY0xWu+P8/z8+XO7rtpad2ZhkEW2IRKV8eHDB0lHPWpEagAopRxPx/v371eKUYIsEWWPWo6jl6OWUq7rRGCuERE26uhTuYLBiExF3Gic50lSK7OU1LQVEU9Pz35DwbbkV9pqA6OUlGspBRtkvVq3kiRIyQwUgOQYg2Ry1VLlZZvky8vI1Bd5v9kFGtYOBAMCDAJ21syUjTAMG0wkUTOv3oMsYCkFlq2IMsYEowQYKMGIiAiS5P1SSo0yGYjwLqLqTXV9wWBjSe01wFGKoF2lvQ2nRUsrc+XrUCrTmZJspbSk3MStNwnNLxlsoY8ZRiGzFillBWP0bqUJaWchIWFTMlNK54IMEQJkGfX5q6ddlb7z5ZQIjDEoB1FXSSdgIvocc04GLW9gSimlxHEc74MRLGQh74ukp1x//PGHMUZvvbWr935e7ePnxxpj9gEpgjMoiwTND398+PnXX+wMIEqZc8I08O7l5btvvxtztKtd5/X5fFztejzOx3nWb77+urVWyaAJr7WOEsseY0AmwaCsIG2U0v73+++FqCVqKZJklChPx/H+/bvndRQGAdNRgiQj6q1fAEH7lmNJfQ6nNkmkJGmjHk9jzIBXsJSAITkivnr+SkpJ0s68JG3M620dt3nYJkzZvXdJZGybIUj76emp90a4MGoJm6ksES/95U25yU1cbpmvIEy8etyt/7bnSsGEttLRADDm7H0QLuQqZRdaRIwxDfLO8fYOwwii7jgM8rVogFud444ICyYsrFTrI2DC21adAjHmILZjwnYABRFgYVTPvHHLtdbKnJv1X1oHDb2qwdVaJTYdDFMmOfqQlG/wS2+T+u75OV5NXLIy11p9zjnXa262mu26cR9n4hYH2wSC3J1DSmutV3Pzlo/6wz//dY3+OM/H+bjO63E+zuv69Dh773PM1lobo139cZ6fH5/n7HPO3OwN2qYRwY+fPv3080+WSJZyrLWkJFGPo777+m9x1Z3zwigRtdZSa2+tt1b2SVNr1l7KHBhjbk2JCBiwgyz18dvvv9GutZZySAYUwefnl/rKzj/d/BZ8eBuclFvoDUnqfWy5ji2YNsljHKNPOOfM48itnwyQUbmp4luu9+luqsHg2/ye9DFg02CE9idErbX1QSsiV87deBEM1rr7NeCuj3jtCO/WByjBQuy+0/DKtE0T2o5jAONYrfeASwRXwJuxLOWoW283923rdkHAhrY5StLWj90uQG/N5K0DssackLYLbXYBrKXWV+dzprbJppW4PTaltNO6txNp6gb0z0ZLUBvX1q4SBdLGs9ZS77bgbppRXl1w99j17WEEGW/1+iYsgAFJYwzLEVG43vq+43j6P7S1uJesljbPAAAAAElFTkSuQmCC';async function showDTC(){const modal=document.getElementById('dtcModal');const list=document.getElementById('dtcList');modal.style.display='flex';list.innerHTML='<li class="dtc-empty">Reading codes...</li>';try{const r=await fetch('/api/dtc');const d=await r.json();if(!d.connected){list.innerHTML='<li class="dtc-empty">OBD not connected</li>';}else if(d.codes.length===0){list.innerHTML='<li class="dtc-empty">No trouble codes found</li>';}else{list.innerHTML=d.codes.map(c=>'<li class="dtc-item">'+c+'</li>').join('');}}catch(e){list.innerHTML='<li class="dtc-empty">Error reading codes</li>';}}function closeDTC(){document.getElementById('dtcModal').style.display='none';}async function showOBD(){document.getElementById('obdModal').style.display='flex';try{const r=await fetch('/api/obd/status');const d=await r.json();document.getElementById('obdStatus').textContent=d.connected?'CONNECTED':'DISCONNECTED';document.getElementById('obdStatus').style.color=d.connected?'#0f6':'#f55';document.getElementById('obdDevice').textContent=d.deviceName||'--';document.getElementById('obdAddr').textContent=d.deviceAddr||'--';document.getElementById('obdWifiPass').value=d.wifiPassword||'';document.getElementById('obdFilterInput').value=d.obdFilter||'';document.getElementById('obdCylinders').value=d.cylinders||6;document.getElementById('obdTankCap').value=d.tankCapacity||16.0;document.getElementById('obdFixedMPG').value=d.fixedMPG||23.0;document.getElementById('obdRefinedMPG').textContent=(d.mapMPG>0?d.mapMPG.toFixed(1)+' MPG':'No data yet');currentAddr=d.deviceAddr||'';renderDevices(d.devices||[]);}catch(e){console.error(e);}}function closeOBD(){document.getElementById('obdModal').style.display='none';}let currentAddr='';function renderDevices(devices){const list=document.getElementById('deviceList');if(!devices.length){list.innerHTML='<div class="dtc-empty">Click Scan to find devices</div>';return;}list.innerHTML=devices.map(d=>{const isConn=(d.addr===currentAddr);return '<div class="device-item '+(d.match?'device-match':'device-nomatch')+'"><div><span class="device-name">'+d.name+'</span><br><span class="device-addr">'+d.addr+'</span></div>'+(isConn?'<span style="color:#0f6;font-size:11px">CONNECTED</span>':'<button class="obd-btn obd-btn-blue" onclick="obdConnect(\''+d.addr+'\')">Connect</button>')+'</div>';}).join('');}async function obdScan(){document.getElementById('deviceList').innerHTML='<div class="dtc-empty">Scanning...</div>';try{const r=await fetch('/api/obd/scan');const d=await r.json();renderDevices(d);}catch(e){document.getElementById('deviceList').innerHTML='<div class="dtc-empty">Scan failed</div>';}}async function obdConnect(addr){document.getElementById('deviceList').innerHTML='<div class="dtc-empty">Connecting...</div>';try{const r=await fetch('/api/obd/connect?addr='+encodeURIComponent(addr));const d=await r.json();if(d.success){showOBD();}else{document.getElementById('deviceList').innerHTML='<div class="dtc-empty">Connection failed</div>';}}catch(e){}}async function obdDisconnect(){try{await fetch('/api/obd/disconnect');showOBD();}catch(e){}}async function obdSave(){const pass=document.getElementById('obdWifiPass').value;const filter=document.getElementById('obdFilterInput').value;const cyl=document.getElementById('obdCylinders').value;const tank=document.getElementById('obdTankCap').value;const mpg=document.getElementById('obdFixedMPG').value;try{await fetch('/api/obd/settings?wifiPassword='+encodeURIComponent(pass)+'&obdFilter='+encodeURIComponent(filter)+'&cylinders='+cyl+'&tankCapacity='+tank+'&fixedMPG='+mpg);await showOBD();}catch(e){}}async function resetMPG(){if(confirm('Clear all MPG data and start fresh?')){try{await fetch('/api/mpg/reset');await showOBD();alert('MPG data cleared');}catch(e){alert('Reset failed');}}}async function updateStatus(){try{const r=await fetch('/api/control/status');const d=await r.json();document.getElementById('statAlarm').textContent=d.alarmArmed?(d.alarmTriggered?'TRIGGERED!':'ARMED'):'DISARMED';document.getElementById('statAlarm').style.color=d.alarmTriggered?'#f55':(d.alarmArmed?'#0f6':'#888');const engineOnline=d.obdConnected&&d.rpm>0;document.getElementById('statEngine').textContent=engineOnline?'ONLINE':'OFFLINE';document.getElementById('statEngine').style.color=engineOnline?'#0f6':'#888';}catch(e){}}function toggleDrawer(){const drawer=document.getElementById('drawer');const btn=document.getElementById('controlsBtn');drawer.classList.toggle('open');btn.classList.toggle('drawer-open');if(drawer.classList.contains('open')){updateStatus();setInterval(updateStatus,1000);}}async function sendLock(){try{await fetch('/api/control/lock');updateStatus();}catch(e){}}async function sendUnlock(){try{await fetch('/api/control/unlock');updateStatus();}catch(e){}}let domeLightOn=false;async function toggleDomeLight(){domeLightOn=!domeLightOn;const btn=document.getElementById('domeLightBtn');try{await fetch('/api/control/domelight?state='+(domeLightOn?'1':'0'));btn.style.borderColor=domeLightOn?'#0f6':'#333';btn.style.boxShadow=domeLightOn?'0 0 25px rgba(0,255,102,0.6)':'0 0 15px rgba(0,0,0,0.5)';}catch(e){}}async function sendRemoteStart(){try{const r=await fetch('/api/control/remotestart');const d=await r.json();if(!d.success){alert('Remote Start Failed: '+d.error);}updateStatus();}catch(e){alert('Remote Start Failed: Network Error');}}let touchStartX=0;let touchStartY=0;const drawer=document.getElementById('drawer');const controlsBtn=document.getElementById('controlsBtn');const dtcModal=document.getElementById('dtcModal');const obdModal=document.getElementById('obdModal');document.body.addEventListener('touchstart',function(e){touchStartX=e.touches[0].clientX;touchStartY=e.touches[0].clientY;},{passive:true});document.body.addEventListener('touchend',function(e){const touchEndX=e.changedTouches[0].clientX;const touchEndY=e.changedTouches[0].clientY;const deltaX=touchEndX-touchStartX;const deltaY=touchEndY-touchStartY;const isHorizontal=Math.abs(deltaX)>Math.abs(deltaY);const isDrawerOpen=drawer.classList.contains('open');const isDTCOpen=dtcModal.style.display==='flex';const isOBDOpen=obdModal.style.display==='flex';if(isHorizontal){if(isDTCOpen&&deltaX>50){closeDTC();}else if(isDrawerOpen&&deltaX>50){drawer.classList.remove('open');controlsBtn.classList.remove('drawer-open');}else if(!isDrawerOpen&&!isDTCOpen&&!isOBDOpen&&deltaX<-50){drawer.classList.add('open');controlsBtn.classList.add('drawer-open');updateDrawerInfo();}}},{passive:true});</script></body></html>)rawliteral";

// BLE notification callback
static void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    for (int i = 0; i < length; i++) {
        char c = (char)pData[i];
        if (c == '>') {
            // Response complete
        } else if (c != '\r' && c != '\n') {
            bleResponse += c;
        }
    }
}

// Check if device name matches any filter keyword
bool matchesFilter(String name) {
    String filter = String(obdFilter);
    int start = 0;
    while (start < filter.length()) {
        int end = filter.indexOf(',', start);
        if (end < 0) end = filter.length();
        String keyword = filter.substring(start, end);
        keyword.trim();
        if (keyword.length() > 0 && name.indexOf(keyword) >= 0) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

// Scan and build JSON of found devices
void scanBLEDevices() {
    foundDevicesJson = "[";
    pBLEScan->start(3, false);
    NimBLEScanResults results = pBLEScan->getResults(0, false);
    bool first = true;
    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice* pDevice = results.getDevice(i);
        String name = pDevice->getName().c_str();
        if (name.length() == 0) continue;
        String addr = pDevice->getAddress().toString().c_str();
        if (!first) foundDevicesJson += ",";
        foundDevicesJson += "{\"name\":\"" + name + "\",\"addr\":\"" + addr + "\",\"match\":" + (matchesFilter(name) ? "true" : "false") + "}";
        first = false;
    }
    foundDevicesJson += "]";
    pBLEScan->clearResults();
}

bool connectToELM() {
    server.handleClient();  // Keep WiFi responsive
    yield();
    NimBLEScanResults results = pBLEScan->getResults(3000, false);  // Shorter scan
    server.handleClient();  // Keep WiFi responsive

    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice* pDevice = results.getDevice(i);
        String name = pDevice->getName().c_str();

        // Look for devices matching the filter
        if (matchesFilter(name)) {

            pClient = NimBLEDevice::createClient();
            pClient->setClientCallbacks(new ClientCallbacks());
            if (pClient->connect(pDevice)) {
                connectedDeviceName = name;
                connectedDeviceAddr = pDevice->getAddress().toString().c_str();

                // Print all services and characteristics for debugging
                const std::vector<NimBLERemoteService*>& services = pClient->getServices(true);
                for (auto pSvc : services) {
                    const std::vector<NimBLERemoteCharacteristic*>& chars = pSvc->getCharacteristics(true);
                    for (auto ch : chars) {
                    }
                }

                // Try to find service
                NimBLERemoteService* pService = pClient->getService(serviceUUID);
                if (pService == nullptr) {
                    // Try alternate UUID
                    pService = pClient->getService(NimBLEUUID("ffe0"));
                }
                if (pService == nullptr) {
                    // Use first available service if standard ones not found
                    if (services.size() > 0) {
                        pService = services.at(0);
                    }
                }
                if (pService == nullptr) {
                    pClient->disconnect();
                    continue;
                }

                // Get characteristics - try multiple known UUIDs
                const std::vector<NimBLERemoteCharacteristic*>& chars = pService->getCharacteristics(true);
                for (auto ch : chars) {
                    if (ch->canWrite() && pTXChar == nullptr) {
                        pTXChar = ch;
                    }
                    if ((ch->canNotify() || ch->canIndicate() || ch->canRead()) && pRXChar == nullptr) {
                        pRXChar = ch;
                    }
                }

                if (pTXChar && pRXChar) {
                    if (pRXChar->canNotify()) {
                        pRXChar->subscribe(true, notifyCallback);
                    } else if (pRXChar->canIndicate()) {
                        pRXChar->subscribe(false, notifyCallback);
                    } else {
                    }
                    pBLEScan->stop();
                    return true;
                } else {
                }
            }
        }
    }
    pBLEScan->clearResults();
    return false;
}

String sendELM(String cmd) {
    if (!obdConnected || !pTXChar) {
        return "";
    }

    bleResponse = "";
    String cmdWithCR = cmd + "\r";
    pTXChar->writeValue((uint8_t*)cmdWithCR.c_str(), cmdWithCR.length());

    unsigned long start = millis();
    while (millis() - start < 500) {
        delay(10);
        server.handleClient();  // Keep WiFi responsive during BLE wait
        yield();
        // Only poll-read if notifications aren't subscribed
        if (pRXChar && !pRXChar->canNotify() && pRXChar->canRead()) {
            std::string val = pRXChar->readValue();
            if (val.length() > 0) {
                for (size_t i = 0; i < val.length(); i++) {
                    char c = val[i];
                    if (c != '\r' && c != '\n') {
                        bleResponse += c;
                    }
                }
            }
        }
        if (bleResponse.length() > 0 && bleResponse.indexOf(">") >= 0) break;
    }


    String resp = bleResponse;
    resp.trim();
    return resp;
}

void initELM() {
    sendELM("ATZ");    delay(500);   // Reset - need longer delay
    sendELM("ATE0");   // Echo off
    sendELM("ATL0");   // Linefeeds off
    sendELM("ATS0");   // Spaces off
    sendELM("ATH0");   // Headers off
    sendELM("ATSP0");  // Auto protocol
    sendELM("ATST32"); // Set timeout (50 * 4ms = 200ms)
    // Try to connect to vehicle
    String resp = sendELM("0100");   // Query supported PIDs
}

int parseHex(String s, int start, int len) {
    if (start + len > s.length()) return 0;
    String hex = s.substring(start, start + len);
    return (int)strtol(hex.c_str(), NULL, 16);
}

void queryOBD() {
    if (!obdConnected) return;

    String resp;
    switch (currentPID) {
        case 0:
            resp = sendELM("010D");
            if (resp.indexOf("410D") >= 0) {
                int idx = resp.indexOf("410D") + 4;
                speed = parseHex(resp, idx, 2);
            }
            break;
        case 1:
            resp = sendELM("010C");
            if (resp.indexOf("410C") >= 0) {
                int idx = resp.indexOf("410C") + 4;
                int a = parseHex(resp, idx, 2);
                int b = parseHex(resp, idx + 2, 2);
                rpm = ((a * 256) + b) / 4.0;
            }
            break;
        case 2:
            resp = sendELM("0105");
            if (resp.indexOf("4105") >= 0) {
                int idx = resp.indexOf("4105") + 4;
                coolantTemp = parseHex(resp, idx, 2) - 40;
            }
            break;
        case 3:
            resp = sendELM("012F");
            if (resp.indexOf("412F") >= 0) {
                int idx = resp.indexOf("412F") + 4;
                fuelLevel = parseHex(resp, idx, 2) * 100.0 / 255.0;
            }
            break;
        case 4:
            resp = sendELM("0104");
            if (resp.indexOf("4104") >= 0) {
                int idx = resp.indexOf("4104") + 4;
                engineLoad = parseHex(resp, idx, 2) * 100.0 / 255.0;
            }
            break;
        case 5:
            resp = sendELM("0142");
            if (resp.indexOf("4142") >= 0) {
                int idx = resp.indexOf("4142") + 4;
                int a = parseHex(resp, idx, 2);
                int b = parseHex(resp, idx + 2, 2);
                voltage = ((a * 256) + b) / 1000.0;
            }
            break;
        case 6:
            resp = sendELM("010F");
            if (resp.indexOf("410F") >= 0) {
                int idx = resp.indexOf("410F") + 4;
                intakeTemp = parseHex(resp, idx, 2) - 40;
            }
            break;
        case 7:
            resp = sendELM("0111");
            if (resp.indexOf("4111") >= 0) {
                int idx = resp.indexOf("4111") + 4;
                throttle = parseHex(resp, idx, 2) * 100.0 / 255.0;
            }
            break;
        case 8:
            resp = sendELM("010B");
            if (resp.indexOf("410B") >= 0) {
                int idx = resp.indexOf("410B") + 4;
                mapPressure = parseHex(resp, idx, 2);  // kPa
            }
            break;
        case 9:
            resp = sendELM("0101");
            if (resp.indexOf("4101") >= 0) {
                int idx = resp.indexOf("4101") + 4;
                int a = parseHex(resp, idx, 2);
                checkEngine = (a & 0x80) != 0;
            }
            break;
    }
    currentPID = (currentPID + 1) % 10;

    float speedMPH = speed * 0.621371;

    // Calculate economy score: speed / load ratio
    // 0 speed + 0 load = midpoint (50)
    // High speed + low load = green (100)
    // Low speed + high load = red (0)
    if (speedMPH < 0.5 && engineLoad < 1.0) {
        economyScore = 50;  // Midpoint when stopped
    } else {
        // Ratio: speed / load (higher = more efficient)
        float ratio = speedMPH / (engineLoad + 0.1);
        // Scale ratio to 0-100 (typical ratio range: 0 to 2.0)
        economyScore = ratio * 50.0;
        if (economyScore > 100) economyScore = 100;
        if (economyScore < 0) economyScore = 0;
    }

    // Actual MPG calculation: fill-to-fill average
    // Check for fill on boot (power on + fuel increase + stationary)
    if (bootFillCheck) {
        // Track stationary state
        if (speedMPH < 2.0) {
            if (!wasStationary) {
                stationaryStartTime = millis();
                wasStationary = true;
            }
        } else {
            wasStationary = false;
        }

        bool stationaryFor5Sec = wasStationary && ((millis() - stationaryStartTime) >= 5000);
        float fuelIncrease = fuelLevel - lastFuelLevel;

        // Fill detection: power on + fuel increase + stationary 5 sec
        if (stationaryFor5Sec && fuelIncrease > 5.0) {  // Fuel increased >5% (real refill)
            if (totalMiles > 0.5) {  // Valid trip data
                float gallonsAdded = (fuelIncrease / 100.0) * tankCapacity;
                float intervalMPG = totalMiles / gallonsAdded;

                if (intervalMPG > 5.0 && intervalMPG < 99.0) {  // Sanity check
                    // Update running average (simple average)
                    if (mapMPG == 0) {
                        mapMPG = intervalMPG;  // First fill
                    } else {
                        mapMPG = (mapMPG + intervalMPG) / 2.0;  // Average of old and new
                    }


                    // Update MPG Setting to track refined average
                    fixedMPG = mapMPG;

                    // Save to NVS
                    preferences.begin("zs-dash", false);
                    preferences.putFloat("map_mpg", mapMPG);
                    preferences.putFloat("fixed_mpg", fixedMPG);  // Track refined average
                    preferences.putFloat("total_miles", 0);
                    preferences.putFloat("last_fuel", fuelLevel);
                    preferences.end();

                    // Reset for next interval
                    totalMiles = 0;
                    lastSavedMiles = 0;
                    lastMilesTime = millis();  // Sync timestamp
                }
            }
            bootFillCheck = false;  // Only check once per boot
            wasStationary = false;
        }
    }

    // Accumulate miles traveled
    if (lastMilesTime > 0) {
        float deltaT_seconds = (millis() - lastMilesTime) / 1000.0;
        if (speedMPH > 2.0) {  // Only count when moving
            totalMiles += (speedMPH / 3600.0) * deltaT_seconds;

            // Save to NVS every +1 mile
            if (totalMiles >= lastSavedMiles + 1.0) {
                preferences.begin("zs-dash", false);
                preferences.putFloat("total_miles", totalMiles);
                preferences.putFloat("last_fuel", fuelLevel);
                preferences.end();
                lastSavedMiles = totalMiles;
            }
        }
    }
    lastMilesTime = millis();

    // Calculate miles till empty using fixed MPG setting
    if (fuelLevel > 0 && fixedMPG > 0) {
        float gallonsRemaining = (fuelLevel / 100.0) * tankCapacity;
        milesTillEmpty = gallonsRemaining * fixedMPG;
    } else {
        milesTillEmpty = 0;
    }
}

// Helper function to extract query parameter value
String getQueryParam(String query, String param) {
    int pos = query.indexOf(param + "=");
    if (pos < 0) return "";
    String value = query.substring(pos + param.length() + 1);
    int ampPos = value.indexOf('&');
    if (ampPos > 0) value = value.substring(0, ampPos);
    // URL decode
    value.replace("%20", " ");
    value.replace("%2C", ",");
    return value;
}

// JSON Builder Functions for Serial Interface

String buildDataJSON() {
    String json = "{";
    json += "\"speed\":" + String(speed, 1) + ",";
    json += "\"rpm\":" + String(rpm, 0) + ",";
    json += "\"coolant\":" + String(coolantTemp, 1) + ",";
    json += "\"fuel\":" + String(fuelLevel, 1) + ",";
    json += "\"load\":" + String(engineLoad, 1) + ",";
    json += "\"voltage\":" + String(voltage, 1) + ",";
    json += "\"intake\":" + String(intakeTemp, 1) + ",";
    json += "\"throttle\":" + String(throttle, 1) + ",";
    json += "\"economy\":" + String(economyScore, 1) + ",";
    json += "\"mapMpg\":" + String(mapMPG, 1) + ",";
    json += "\"tripMiles\":" + String(totalMiles, 1) + ",";
    json += "\"mte\":" + String(milesTillEmpty, 0) + ",";
    json += "\"checkEngine\":" + String(checkEngine ? "true" : "false") + ",";
    json += "\"obdConnected\":" + String(obdConnected ? "true" : "false");
    json += "}";
    return json;
}

String buildDTCJSON() {
    String json = "{\"codes\":[";

    if (obdConnected) {
        String resp = sendELM("03");

        if (resp.indexOf("43") >= 0) {
            int idx = resp.indexOf("43");
            resp.replace(" ", "");
            resp = resp.substring(idx + 2);

            if (resp.length() >= 2) {
                int count = parseHex(resp, 0, 2);

                bool first = true;
                for (int i = 0; i < count && (i * 4 + 2) < resp.length(); i++) {
                    int codeIdx = 2 + (i * 4);
                    if (codeIdx + 4 <= resp.length()) {
                        String codeHex = resp.substring(codeIdx, codeIdx + 4);
                        if (codeHex != "0000") {
                            char firstChar = codeHex.charAt(0);
                            String dtcType = getDTCType(firstChar);
                            String dtcNum = codeHex.substring(1);

                            if (!first) json += ",";
                            json += "\"" + dtcType + dtcNum + "\"";
                            first = false;
                        }
                    }
                }
            }
        }
    }

    json += "],\"connected\":" + String(obdConnected ? "true" : "false") + "}";
    return json;
}

String buildOBDStatusJSON() {
    String devicesJson = foundDevicesJson;
    if (obdConnected && connectedDeviceName.length() > 0 && foundDevicesJson == "[]") {
        devicesJson = "[{\"name\":\"" + connectedDeviceName + "\",\"addr\":\"" + connectedDeviceAddr + "\",\"match\":true}]";
    }
    String json = "{";
    json += "\"connected\":" + String(obdConnected ? "true" : "false") + ",";
    json += "\"deviceName\":\"" + connectedDeviceName + "\",";
    json += "\"deviceAddr\":\"" + connectedDeviceAddr + "\",";
    json += "\"wifiSSID\":\"" + String(ssid) + "\",";
    json += "\"wifiPassword\":\"" + String(wifiPassword) + "\",";
    json += "\"obdFilter\":\"" + String(obdFilter) + "\",";
    json += "\"cylinders\":" + String(engineCylinders) + ",";
    json += "\"tankCapacity\":" + String(tankCapacity, 1) + ",";
    json += "\"fixedMPG\":" + String(fixedMPG, 1) + ",";
    json += "\"mapMPG\":" + String(mapMPG, 1) + ",";
    json += "\"devices\":" + devicesJson;
    json += "}";
    return json;
}

String buildOBDScanJSON() {
    scanBLEDevices();
    return foundDevicesJson;
}

String buildOBDConnectJSON(String addr) {
    // Scan and find device by address
    pBLEScan->start(3, false);
    NimBLEScanResults results = pBLEScan->getResults(0, false);
    bool found = false;

    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice* pDevice = results.getDevice(i);
        String devAddr = pDevice->getAddress().toString().c_str();
        if (devAddr.equalsIgnoreCase(addr)) {
            // Disconnect existing if any
            if (pClient && pClient->isConnected()) {
                pClient->disconnect();
            }
            pTXChar = nullptr;
            pRXChar = nullptr;

            pClient = NimBLEDevice::createClient();
            pClient->setClientCallbacks(new ClientCallbacks());
            if (pClient->connect(pDevice)) {
                connectedDeviceName = pDevice->getName().c_str();
                connectedDeviceAddr = devAddr;

                // Find service and characteristics (simplified)
                const std::vector<NimBLERemoteService*>& services = pClient->getServices(true);
                for (auto pSvc : services) {
                    const std::vector<NimBLERemoteCharacteristic*>& chars = pSvc->getCharacteristics(true);
                    for (auto ch : chars) {
                        if (ch->canWrite() && pTXChar == nullptr) pTXChar = ch;
                        if ((ch->canNotify() || ch->canRead()) && pRXChar == nullptr) pRXChar = ch;
                    }
                    if (pTXChar && pRXChar) break;
                }

                if (pTXChar && pRXChar) {
                    if (pRXChar->canNotify()) pRXChar->subscribe(true, notifyCallback);
                    obdConnected = true;
                    initELM();
                    found = true;
                }
            }
            break;
        }
    }
    pBLEScan->clearResults();

    return "{\"success\":" + String(found ? "true" : "false") + ",\"connected\":" + String(obdConnected ? "true" : "false") + "}";
}

String buildOBDDisconnectJSON() {
    if (obdConnected && pClient) {
        pClient->disconnect();
    }
    obdConnected = false;
    pTXChar = nullptr;
    pRXChar = nullptr;
    connectedDeviceName = "";
    connectedDeviceAddr = "";
    return "{\"success\":true}";
}

String buildOBDSettingsJSON(String params) {
    String wifiPasswordParam = getQueryParam(params, "wifiPassword");
    String obdFilterParam = getQueryParam(params, "obdFilter");
    String cylindersParam = getQueryParam(params, "cylinders");
    String tankCapacityParam = getQueryParam(params, "tankCapacity");
    String fixedMPGParam = getQueryParam(params, "fixedMPG");

    if (wifiPasswordParam.length() > 0) {
        strncpy(wifiPassword, wifiPasswordParam.c_str(), sizeof(wifiPassword) - 1);
        wifiPassword[sizeof(wifiPassword) - 1] = '\0';
        preferences.begin("dashboard", false);
        preferences.putString("wifiPassword", wifiPasswordParam);
        preferences.end();
    }
    
    if (obdFilterParam.length() > 0) {
        strncpy(obdFilter, obdFilterParam.c_str(), sizeof(obdFilter) - 1);
        obdFilter[sizeof(obdFilter) - 1] = '\0';
        preferences.begin("dashboard", false);
        preferences.putString("obdFilter", obdFilterParam);
        preferences.end();
    }
    
    if (cylindersParam.length() > 0) {
        engineCylinders = cylindersParam.toInt();
        if (engineCylinders == 4) {
            engineDisplacement = 2.0;
        } else if (engineCylinders == 6) {
            engineDisplacement = 3.0;
        } else if (engineCylinders == 8) {
            engineDisplacement = 4.0;
        }
        preferences.begin("dashboard", false);
        preferences.putInt("cylinders", engineCylinders);
        preferences.putFloat("displacement", engineDisplacement);
        preferences.end();
    }
    
    if (tankCapacityParam.length() > 0) {
        tankCapacity = tankCapacityParam.toFloat();
        preferences.begin("dashboard", false);
        preferences.putFloat("tankCapacity", tankCapacity);
        preferences.end();
    }
    
    if (fixedMPGParam.length() > 0) {
        fixedMPG = fixedMPGParam.toFloat();
        preferences.begin("dashboard", false);
        preferences.putFloat("fixedMPG", fixedMPG);
        preferences.end();
    }

    return "{\"success\":true}";
}

String buildLockJSON() {
    alarmArmed = true;
    digitalWrite(PIN_LOCK, RELAY_ON);
    delay(RELAY_SETTLE_DELAY);
    digitalWrite(PIN_LOCK, RELAY_OFF);
    return "{\"success\":true,\"action\":\"lock\"}";
}

String buildUnlockJSON() {
    alarmArmed = false;
    alarmTriggered = false;
    digitalWrite(PIN_SIREN, RELAY_OFF);
    sirenActive = false;
    digitalWrite(PIN_UNLOCK, RELAY_ON);
    delay(RELAY_SETTLE_DELAY);
    digitalWrite(PIN_UNLOCK, RELAY_OFF);
    if (remoteStartActive) {
        digitalWrite(PIN_ACCESSORY, RELAY_OFF);
        digitalWrite(PIN_IGNITION, RELAY_OFF);
        digitalWrite(PIN_STARTER, RELAY_OFF);
        remoteStartActive = false;
    }
    return "{\"success\":true,\"action\":\"unlock\"}";
}

String buildDomeLightJSON(String params) {
    String state = getQueryParam(params, "state");
    if (state == "1") {
        digitalWrite(PIN_ACCESSORY, RELAY_ON);
        domeLightOn = true;
    } else {
        if (!remoteStartActive) {
            digitalWrite(PIN_ACCESSORY, RELAY_OFF);
        }
        domeLightOn = false;
    }
    return "{\"success\":true,\"state\":" + String(domeLightOn ? "true" : "false") + "}";
}

String buildRemoteStartJSON() {
    if (!obdConnected) {
        return "{\"success\":false,\"error\":\"OBD not connected\"}";
    }
    
    if (remoteStartActive) {
        return "{\"success\":false,\"error\":\"Remote start already active\"}";
    }

    int attempts = 0;
    bool started = false;

    while (attempts < MAX_START_ATTEMPTS && !started) {
        attempts++;

        digitalWrite(PIN_ACCESSORY, RELAY_ON);
        delay(ECM_BOOT_WAIT);

        if (!obdConnected) {
            return "{\"success\":false,\"error\":\"Lost OBD connection during start\"}";
        }

        digitalWrite(PIN_IGNITION, RELAY_ON);
        delay(RELAY_SETTLE_DELAY);
        digitalWrite(PIN_STARTER, RELAY_ON);

        unsigned long crankStart = millis();
        while (millis() - crankStart < STARTER_MAX_CRANK) {
            queryOBD();

            if (rpm >= RPM_RUNNING_THRESHOLD) {
                started = true;
                break;
            }

            delay(STARTER_POLL_INTERVAL);
        }

        digitalWrite(PIN_STARTER, RELAY_OFF);

        if (started) {
            break;
        }

        digitalWrite(PIN_IGNITION, RELAY_OFF);
        digitalWrite(PIN_ACCESSORY, RELAY_OFF);
        delay(3000);
    }

    if (!started) {
        digitalWrite(PIN_IGNITION, RELAY_OFF);
        digitalWrite(PIN_ACCESSORY, RELAY_OFF);
        return "{\"success\":false,\"error\":\"Failed to start after " + String(MAX_START_ATTEMPTS) + " attempts\"}";
    }

    remoteStartActive = true;
    remoteStartTimer = millis();
    engineRunning = true;

    return "{\"success\":true,\"attempts\":" + String(attempts) + "}";
}

String buildStatusJSON() {
    String json = "{";
    json += "\"alarmArmed\":" + String(alarmArmed ? "true" : "false") + ",";
    json += "\"alarmTriggered\":" + String(alarmTriggered ? "true" : "false") + ",";
    json += "\"engineRunning\":" + String(engineRunning ? "true" : "false") + ",";
    json += "\"remoteStartActive\":" + String(remoteStartActive ? "true" : "false") + ",";
    json += "\"obdConnected\":" + String(obdConnected ? "true" : "false") + ",";
    json += "\"rpm\":" + String(rpm, 0);
    json += "}";
    return json;
}

String buildMPGResetJSON() {
    totalMiles = 0;
    lastFuelLevel = fuelLevel;
    lastMilesTime = millis();
    lastSavedMiles = 0;
    stationaryStartTime = 0;
    wasStationary = false;
    bootFillCheck = true;

    preferences.begin("dashboard", false);
    preferences.putFloat("totalMiles", 0);
    preferences.putFloat("lastFuelLevel", fuelLevel);
    preferences.putULong("lastMilesTime", millis());
    preferences.end();

    return "{\"success\":true}";
}


void handleRoot() {
    server.send(200, "text/html", html);
}

void handleData() {
    String json = buildDataJSON();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

String getDTCType(char c) {
    switch(c) {
        case '0': return "P0";
        case '1': return "P1";
        case '2': return "P2";
        case '3': return "P3";
        case '4': return "C0";
        case '5': return "C1";
        case '6': return "C2";
        case '7': return "C3";
        case '8': return "B0";
        case '9': return "B1";
        case 'A': return "B2";
        case 'B': return "B3";
        case 'C': return "U0";
        case 'D': return "U1";
        case 'E': return "U2";
        case 'F': return "U3";
        default: return "??";
    }
}

void handleDTC() {
    String json = buildDTCJSON();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleOBDStatus() {
    String json = buildOBDStatusJSON();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleOBDScan() {
    String json = buildOBDScanJSON();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleOBDConnect() {
    String addr = server.arg("addr");
    String json = buildOBDConnectJSON(addr);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleOBDDisconnect() {
    String json = buildOBDDisconnectJSON();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleOBDSettings() {
    String params = "";
    if (server.hasArg("wifiPassword")) {
        params += "wifiPassword=" + server.arg("wifiPassword") + "&";
    }
    if (server.hasArg("obdFilter")) {
        params += "obdFilter=" + server.arg("obdFilter") + "&";
    }
    if (server.hasArg("cylinders")) {
        params += "cylinders=" + server.arg("cylinders") + "&";
    }
    if (server.hasArg("tankCapacity")) {
        params += "tankCapacity=" + server.arg("tankCapacity") + "&";
    }
    if (server.hasArg("fixedMPG")) {
        params += "fixedMPG=" + server.arg("fixedMPG") + "&";
    }

    String json = buildOBDSettingsJSON(params);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleLock() {
    String json = buildLockJSON();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleUnlock() {
    String json = buildUnlockJSON();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleDomeLight() {
    String params = "";
    if (server.hasArg("state")) {
        params = "state=" + server.arg("state");
    } else {
        // Toggle mode - determine new state based on current state
        params = "state=" + String(domeLightOn ? "0" : "1");
    }

    String json = buildDomeLightJSON(params);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleRemoteStart() {
    String json = buildRemoteStartJSON();
    int statusCode = (json.indexOf("\"success\":true") >= 0) ? 200 : 400;
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(statusCode, "application/json", json);
}

void handleStatus() {
    String json = buildStatusJSON();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleMPGReset() {
    String json = buildMPGResetJSON();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

// ====================
// MODEM FUNCTIONS
// ====================

// Send modem response with proper formatting
void modemSend(String response) {
    if (modemVerbose) {
        Serial.print("\r\n");
        Serial.print(response);
        Serial.print("\r\n");
    } else {
        // Numeric codes when verbose off
        if (response == "OK") Serial.print("0\r");
        else if (response == "CONNECT") Serial.print("1\r");
        else if (response == "RING") Serial.print("2\r");
        else if (response == "NO CARRIER") Serial.print("3\r");
        else if (response == "ERROR") Serial.print("4\r");
        else if (response == "NO DIALTONE") Serial.print("6\r");
        else if (response == "BUSY") Serial.print("7\r");
        else if (response == "NO ANSWER") Serial.print("8\r");
        else Serial.print(response + "\r");
    }
}

// Load phonebook from Preferences
void loadPhonebook() {
    preferences.begin("modem", true);
    phonebookCount = preferences.getInt("pb_count", 0);
    for (int i = 0; i < phonebookCount && i < MAX_PHONEBOOK_ENTRIES; i++) {
        String prefix = "pb" + String(i) + "_";
        phonebook[i].number = preferences.getString((prefix + "num").c_str(), "");
        phonebook[i].ssid = preferences.getString((prefix + "ssid").c_str(), "");
        phonebook[i].password = preferences.getString((prefix + "pass").c_str(), "");
    }
    preferences.end();
}

// Save phonebook to Preferences
void savePhonebook() {
    preferences.begin("modem", false);
    preferences.putInt("pb_count", phonebookCount);
    for (int i = 0; i < phonebookCount && i < MAX_PHONEBOOK_ENTRIES; i++) {
        String prefix = "pb" + String(i) + "_";
        preferences.putString((prefix + "num").c_str(), phonebook[i].number);
        preferences.putString((prefix + "ssid").c_str(), phonebook[i].ssid);
        preferences.putString((prefix + "pass").c_str(), phonebook[i].password);
    }
    preferences.end();
}

// Find phonebook entry by number
int findPhonebookEntry(String number) {
    for (int i = 0; i < phonebookCount; i++) {
        if (phonebook[i].number == number) {
            return i;
        }
    }
    return -1;
}

// Dial (connect to WiFi)
void modemDial(String number) {
    modemState = MODEM_DIALING;

    // Look up number in phonebook
    int idx = findPhonebookEntry(number);
    if (idx < 0) {
        modemSend("NO CARRIER");
        modemState = MODEM_IDLE;
        return;
    }

    // Try to connect to WiFi
    String ssid = phonebook[idx].ssid;
    String pass = phonebook[idx].password;

    // Switch to AP+STA mode
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    // Wait for connection (max 10 seconds)
    int timeout = 100;  // 10 seconds (100 * 100ms)
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
        delay(100);
        server.handleClient();  // Keep AP responsive
        timeout--;
    }

    if (WiFi.status() == WL_CONNECTED) {
        staConnected = true;
        modemState = MODEM_CONNECTED;
        modemSend("CONNECT 56000");
        ppp_start();  // Start PPP server
    } else {
        WiFi.mode(WIFI_AP);  // Revert to AP-only
        modemSend("NO CARRIER");
        modemState = MODEM_IDLE;
    }
}

// Hangup (disconnect WiFi)
void modemHangup() {
    if (modemState == MODEM_ONLINE) {
        ppp_shutdown();  // Shutdown PPP first
    }
    if (staConnected) {
        WiFi.disconnect();
        WiFi.mode(WIFI_AP);  // Revert to AP-only
        staConnected = false;
    }
    modemState = MODEM_IDLE;
    modemSend("NO CARRIER");
}

// Check for Hayes escape sequence (+++ with guard times)
// Returns true if escape sequence detected and should return to command mode
bool checkEscapeSequence(char ch) {
    unsigned long now = millis();

    // If character is provided, track it
    if (ch != 0) {
        // Check if guard time elapsed since last activity
        if ((long)(now - lastSerialActivity) >= ESC_GUARD_TIME) {
            // Guard time passed, reset counter
            escCounter = 0;
        }

        lastSerialActivity = now;

        // Check if this is an escape character
        if (ch == ESC_CHARACTER) {
            escCounter++;

            // If we have all escape characters, set post-sequence guard time
            if (escCounter >= ESC_TIMES) {
                escCheckTime = now + ESC_GUARD_TIME;
            }
        } else {
            // Non-escape character resets everything
            escCounter = 0;
            escCheckTime = 0;
        }

        return false;  // Not yet complete
    }

    // Called with null character - check if sequence is complete
    if (escCounter == ESC_TIMES && escCheckTime > 0) {
        // Check if post-sequence guard time has elapsed
        if ((long)(now - escCheckTime) >= 0) {
            // Escape sequence complete!
            escCounter = 0;
            escCheckTime = 0;
            return true;
        }
    }

    return false;
}

// Process AT command
void processATCommand(String cmd) {
    cmd.trim();
    // Create uppercase version for command matching, preserve original for parameters
    String cmdUpper = cmd;
    cmdUpper.toUpperCase();

    // Echo command if enabled
    if (modemEcho && cmd.length() > 0) {
        Serial.println(cmd);
    }

    // Handle empty AT
    if (cmdUpper == "AT") {
        modemSend("OK");
        return;
    }

    // ATZ - Reset modem
    if (cmdUpper == "ATZ") {
        modemHangup();
        modemEcho = true;
        modemVerbose = true;
        modemSend("OK");
        return;
    }

    // ATE - Echo control
    if (cmdUpper.startsWith("ATE")) {
        modemEcho = (cmdUpper.charAt(3) == '1');
        modemSend("OK");
        return;
    }

    // ATV - Verbose control
    if (cmdUpper.startsWith("ATV")) {
        modemVerbose = (cmdUpper.charAt(3) == '1');
        modemSend("OK");
        return;
    }

    // ATI - Identification
    if (cmdUpper.startsWith("ATI")) {
        Serial.println("ESP32 WiFi Modem v1.0");
        Serial.println("Z-Car-Dashboard");
        modemSend("OK");
        return;
    }

    // ATH - Hangup
    if (cmdUpper.startsWith("ATH")) {
        modemHangup();
        modemSend("OK");
        return;
    }

    // ATDT - Dial (tone)
    if (cmdUpper.startsWith("ATDT")) {
        String number = cmd.substring(4);
        number.trim();
        if (number.length() > 0) {
            modemDial(number);
        } else {
            modemSend("ERROR");
        }
        return;
    }

    // ATDP - Dial (pulse) - treat same as tone
    if (cmdUpper.startsWith("ATDP")) {
        String number = cmd.substring(4);
        number.trim();
        if (number.length() > 0) {
            modemDial(number);
        } else {
            modemSend("ERROR");
        }
        return;
    }

    // AT+PBLIST - List phonebook
    if (cmdUpper == "AT+PBLIST") {
        for (int i = 0; i < phonebookCount; i++) {
            Serial.print(phonebook[i].number);
            Serial.print(" -> ");
            Serial.println(phonebook[i].ssid);
        }
        modemSend("OK");
        return;
    }

    // AT+PBADD=number,ssid,password - Add phonebook entry
    if (cmdUpper.startsWith("AT+PBADD=")) {
        String params = cmd.substring(9);
        int comma1 = params.indexOf(',');
        int comma2 = params.indexOf(',', comma1 + 1);

        if (comma1 > 0 && comma2 > comma1 && phonebookCount < MAX_PHONEBOOK_ENTRIES) {
            phonebook[phonebookCount].number = params.substring(0, comma1);
            phonebook[phonebookCount].ssid = params.substring(comma1 + 1, comma2);
            phonebook[phonebookCount].password = params.substring(comma2 + 1);
            phonebookCount++;
            savePhonebook();
            modemSend("OK");
        } else {
            modemSend("ERROR");
        }
        return;
    }

    // AT+PBDEL=number - Delete phonebook entry
    if (cmdUpper.startsWith("AT+PBDEL=")) {
        String number = cmd.substring(9);
        int idx = findPhonebookEntry(number);
        if (idx >= 0) {
            // Shift entries down
            for (int i = idx; i < phonebookCount - 1; i++) {
                phonebook[i] = phonebook[i + 1];
            }
            phonebookCount--;
            savePhonebook();
            modemSend("OK");
        } else {
            modemSend("ERROR");
        }
        return;
    }

    // AT+WIFISTATUS - WiFi status
    if (cmdUpper == "AT+WIFISTATUS") {
        Serial.print("AP: ");
        Serial.println(ssid);
        Serial.print("STA: ");
        if (staConnected) {
            Serial.print("Connected to ");
            Serial.print(WiFi.SSID());
            Serial.print(" (");
            Serial.print(WiFi.localIP());
            Serial.println(")");
        } else {
            Serial.println("Disconnected");
        }
        modemSend("OK");
        return;
    }

    // AT+IPR=<baudrate> - Set DTE baud rate (volatile, resets on power cycle)
    if (cmdUpper.startsWith("AT+IPR=")) {
        String rateStr = cmd.substring(7);
        rateStr.trim();

        if (rateStr.length() > 0) {
            unsigned long newRate = rateStr.toInt();

            // Validate baud rate (common rates)
            if (newRate == 300 || newRate == 1200 || newRate == 2400 ||
                newRate == 4800 || newRate == 9600 || newRate == 19200 ||
                newRate == 38400 || newRate == 57600 || newRate == 115200 ||
                newRate == 230400 || newRate == 460800 || newRate == 921600) {

                // Send OK at current baud rate
                modemSend("OK");
                Serial.flush();  // Wait for transmission to complete
                delay(50);       // Extra safety delay

                // Change baud rate
                Serial.end();
                Serial.begin(newRate);

                return;
            }
        }

        modemSend("ERROR");
        return;
    }

    // AT+IPR? - Query current baud rate
    if (cmdUpper == "AT+IPR?") {
        Serial.print("+IPR: ");
        Serial.println(Serial.baudRate());
        modemSend("OK");
        return;
    }

    // Unknown command
    modemSend("ERROR");
}

// ====================
// PPP SERVER IMPLEMENTATION
// ====================

// CRC-16 lookup table for FCS calculation
static const uint16_t fcstab[256] = {
  0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
  0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
  0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
  0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
  0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
  0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
  0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
  0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
  0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
  0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
  0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
  0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
  0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
  0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
  0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
  0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
  0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
  0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
  0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
  0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
  0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
  0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
  0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
  0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
  0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
  0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
  0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
  0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
  0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
  0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
  0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
  0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

// WiFi output hook - intercepts outgoing packets
static err_t ppp_wifi_output_hook(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr) {
    if (original_wifi_output != NULL)
        return original_wifi_output(netif, p, ipaddr);
    return ERR_OK;
}

// WiFi input hook - intercepts incoming packets destined for our IP
static err_t ppp_wifi_input_hook(struct pbuf *p, struct netif *inp) {
    if ((p != NULL) && (p->len >= 34)) {
        uint8_t *payload = (uint8_t *)p->payload;
        uint8_t *data = payload + 14;  // Skip Ethernet header
        uint8_t ip_version = (data[0] >> 4) & 0x0F;

        if (ip_version == 4) {
            uint32_t dest_ip = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
            IPAddress wifiIP = WiFi.localIP();
            uint32_t our_ip = ((uint32_t)wifiIP[0] << 24) | ((uint32_t)wifiIP[1] << 16) |
                             ((uint32_t)wifiIP[2] << 8) | (uint32_t)wifiIP[3];

            if (dest_ip == our_ip) {
                pbuf_remove_header(p, 14);  // Strip Ethernet header
                ppp_sendPacketToSerial(p);
                pbuf_free(p);
                return ERR_OK;
            }
        }
    }

    if (original_wifi_input != NULL)
        return original_wifi_input(p, inp);

    return ERR_OK;
}

// PPP network interface init
static err_t ppp_netif_init(struct netif *netif) {
    netif->name[0] = 'p';
    netif->name[1] = 'p';
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

// Calculate FCS for PPP frame
uint16_t ppp_calculateFCS(uint8_t *data, int len) {
    uint16_t fcs = 0xFFFF;
    for (int i = 0; i < len; i++)
        fcs = (fcs >> 8) ^ fcstab[(fcs ^ data[i]) & 0xFF];
    return fcs ^ 0xFFFF;
}

// Send PPP frame to serial with byte stuffing
void ppp_sendFrame(uint16_t protocol, uint8_t *data, int len) {
    uint8_t header[4];
    header[0] = 0xFF;
    header[1] = 0x03;
    header[2] = (protocol >> 8) & 0xFF;
    header[3] = protocol & 0xFF;

    uint16_t fcs = 0xFFFF;
    for (int i = 0; i < 4; i++)
        fcs = (fcs >> 8) ^ fcstab[(fcs ^ header[i]) & 0xFF];
    for (int i = 0; i < len; i++)
        fcs = (fcs >> 8) ^ fcstab[(fcs ^ data[i]) & 0xFF];
    fcs ^= 0xFFFF;

    Serial.write(PPP_FLAG);
    yield();

    // Send header with byte stuffing
    for (int i = 0; i < 4; i++) {
        uint8_t c = header[i];
        if (c == PPP_FLAG || c == PPP_ESCAPE || c < 0x20) {
            Serial.write(PPP_ESCAPE);
            Serial.write(c ^ PPP_TRANS);
        } else {
            Serial.write(c);
        }
    }

    // Send data with byte stuffing
    for (int i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == PPP_FLAG || c == PPP_ESCAPE || c < 0x20) {
            Serial.write(PPP_ESCAPE);
            Serial.write(c ^ PPP_TRANS);
        } else {
            Serial.write(c);
        }
        if ((i % 64) == 0) yield();
    }

    // Send FCS with byte stuffing
    uint8_t fcs_bytes[2];
    fcs_bytes[0] = fcs & 0xFF;
    fcs_bytes[1] = (fcs >> 8) & 0xFF;
    for (int i = 0; i < 2; i++) {
        uint8_t c = fcs_bytes[i];
        if (c == PPP_FLAG || c == PPP_ESCAPE || c < 0x20) {
            Serial.write(PPP_ESCAPE);
            Serial.write(c ^ PPP_TRANS);
        } else {
            Serial.write(c);
        }
    }

    Serial.write(PPP_FLAG);
    yield();
}

void ppp_sendLCPPacket(uint8_t code, uint8_t id, uint8_t *data, int len) {
    uint8_t packet[1500];
    packet[0] = code;
    packet[1] = id;
    packet[2] = ((len + 4) >> 8) & 0xFF;
    packet[3] = (len + 4) & 0xFF;
    if (len > 0)
        memcpy(packet + 4, data, len);
    ppp_sendFrame(PPP_PROTOCOL_LCP, packet, len + 4);
}

void ppp_sendIPCPPacket(uint8_t code, uint8_t id, uint8_t *data, int len) {
    uint8_t packet[1500];
    packet[0] = code;
    packet[1] = id;
    packet[2] = ((len + 4) >> 8) & 0xFF;
    packet[3] = (len + 4) & 0xFF;
    if (len > 0)
        memcpy(packet + 4, data, len);
    ppp_sendFrame(PPP_PROTOCOL_IPCP, packet, len + 4);
}

void ppp_handleLCP(uint8_t *data, int len) {
    if (len < 4) return;

    uint8_t code = data[0];
    uint8_t id = data[1];

    switch (code) {
        case LCP_CONF_REQ:
            ppp_sendLCPPacket(LCP_CONF_ACK, id, data + 4, len - 4);
            if (!lcpOpened) {
                lcpOpened = true;
                lcpId++;
                ppp_sendLCPPacket(LCP_CONF_REQ, lcpId, NULL, 0);
            }
            break;
        case LCP_CONF_ACK:
            if (lcpOpened)
                pppState = PPP_OPENED;
            break;
        case LCP_ECHO_REQ:
            ppp_sendLCPPacket(LCP_ECHO_REPLY, id, data + 4, len - 4);
            break;
        case LCP_TERM_REQ:
            ppp_sendLCPPacket(LCP_TERM_ACK, id, NULL, 0);
            ppp_shutdown();
            break;
    }
}

void ppp_handleIPCP(uint8_t *data, int len) {
    if (len < 4) return;

    uint8_t code = data[0];
    uint8_t id = data[1];

    switch (code) {
        case LCP_CONF_REQ: {
            uint8_t response[256];
            int respLen = 0;
            int i = 4;

            while (i < len) {
                uint8_t optType = data[i];
                uint8_t optLen = data[i + 1];

                if (optType == 3) {  // IP address option
                    IPAddress wifiIP = WiFi.localIP();
                    response[respLen++] = 3;
                    response[respLen++] = 6;
                    response[respLen++] = wifiIP[0];
                    response[respLen++] = wifiIP[1];
                    response[respLen++] = wifiIP[2];
                    response[respLen++] = wifiIP[3];
                } else {
                    memcpy(response + respLen, data + i, optLen);
                    respLen += optLen;
                }
                i += optLen;
            }

            ppp_sendIPCPPacket(LCP_CONF_ACK, id, response, respLen);

            if (!ipcpOpened) {
                ipcpOpened = true;
                ipcpId++;
                uint8_t ipReq[6];
                IPAddress wifiIP = WiFi.localIP();
                ipReq[0] = 3;
                ipReq[1] = 6;
                ipReq[2] = wifiIP[0];
                ipReq[3] = wifiIP[1];
                ipReq[4] = wifiIP[2];
                ipReq[5] = wifiIP[3];
                ppp_sendIPCPPacket(LCP_CONF_REQ, ipcpId, ipReq, 6);
            }
            break;
        }
        case LCP_CONF_ACK:
            // IP forwarding now active
            break;
    }
}

void ppp_handleIPPacket(uint8_t *data, int len) {
    if (pppState != PPP_OPENED || !ipcpOpened || len < 20)
        return;

    if (wifi_netif == NULL || original_wifi_output == NULL)
        return;

    struct pbuf *p = pbuf_alloc(PBUF_IP, len, PBUF_RAM);
    if (p == NULL)
        return;

    memcpy(p->payload, data, len);

    ip4_addr_t dest;
    IP4_ADDR(&dest, data[16], data[17], data[18], data[19]);

    err_t err = original_wifi_output(wifi_netif, p, &dest);
    if (err != ERR_OK)
        pbuf_free(p);
}

void ppp_processFrame(uint8_t *data, int len) {
    if (len < 6) return;

    uint16_t fcs = ppp_calculateFCS(data, len - 2);
    uint16_t recv_fcs = data[len - 2] | (data[len - 1] << 8);

    if (fcs != recv_fcs) return;

    if (data[0] != 0xFF || data[1] != 0x03) return;

    uint16_t protocol = (data[2] << 8) | data[3];
    uint8_t *payload = data + 4;
    int payloadLen = len - 6;

    switch (protocol) {
        case PPP_PROTOCOL_LCP:
            ppp_handleLCP(payload, payloadLen);
            break;
        case PPP_PROTOCOL_IPCP:
            ppp_handleIPCP(payload, payloadLen);
            break;
        case PPP_PROTOCOL_IP:
            ppp_handleIPPacket(payload, payloadLen);
            break;
    }
}

void ppp_sendPacketToSerial(struct pbuf *p) {
    if (p == NULL || pppState != PPP_OPENED || !ipcpOpened)
        return;

    struct pbuf *q = p;
    int total = 0;
    uint8_t tempBuf[1600];

    while (q != NULL) {
        uint8_t *payload = (uint8_t *)q->payload;
        int len = q->len;
        memcpy(tempBuf + total, payload, len);
        total += len;
        q = q->next;
    }

    if (total > 0)
        ppp_sendFrame(PPP_PROTOCOL_IP, tempBuf, total);
}

void ppp_start() {
    pppCurBufLen = 0;
    pppEscaped = false;
    pppState = PPP_ESTABLISH;
    lcpOpened = false;
    ipcpOpened = false;
    lcpId = 0;
    ipcpId = 0;

    delay(500);
    while (Serial.available() > 0)
        Serial.read();

    // Find WiFi interface
    wifi_netif = netif_list;
    while (wifi_netif != NULL) {
        if ((wifi_netif->name[0] == 's' && wifi_netif->name[1] == 't') ||
            (wifi_netif->name[0] == 'e' && wifi_netif->name[1] == 'n'))
            break;
        wifi_netif = wifi_netif->next;
    }

    if (wifi_netif == NULL)
        wifi_netif = netif_default;

    if (wifi_netif != NULL) {
        original_wifi_output = wifi_netif->output;
        wifi_netif->output = ppp_wifi_output_hook;

        original_wifi_input = wifi_netif->input;
        wifi_netif->input = ppp_wifi_input_hook;
    }

    ip4_addr_t ipaddr, netmask, gw;
    IPAddress wifiIP = WiFi.localIP();
    IPAddress wifiGW = WiFi.gatewayIP();
    IPAddress wifiMask = WiFi.subnetMask();

    if (wifiIP[0] != 0) {
        IP4_ADDR(&ipaddr, wifiIP[0], wifiIP[1], wifiIP[2], wifiIP[3]);
        IP4_ADDR(&gw, wifiGW[0], wifiGW[1], wifiGW[2], wifiGW[3]);
        IP4_ADDR(&netmask, wifiMask[0], wifiMask[1], wifiMask[2], wifiMask[3]);
    } else {
        IP4_ADDR(&ipaddr, 192, 168, 1, 1);
        IP4_ADDR(&netmask, 255, 255, 255, 0);
        IP4_ADDR(&gw, 192, 168, 1, 1);
    }

    netif_add(&ppp_netif, &ipaddr, &netmask, &gw, NULL, ppp_netif_init, ip4_input);
    netif_set_up(&ppp_netif);
    netif_set_link_up(&ppp_netif);

    modemState = MODEM_ONLINE;
}

void ppp_shutdown() {
    if (wifi_netif != NULL && original_wifi_output != NULL)
        wifi_netif->output = original_wifi_output;
    if (wifi_netif != NULL && original_wifi_input != NULL)
        wifi_netif->input = original_wifi_input;

    netif_remove(&ppp_netif);
    modemState = MODEM_IDLE;

    if (pppBuf != NULL) {
        free(pppBuf);
        pppBuf = NULL;
    }
}

void ppp_serialIncoming() {
    while (Serial.available() > 0) {
        uint8_t c = Serial.read();

        // Check for Hayes escape sequence (+++)
        if (checkEscapeSequence((char)c)) {
            // Escape sequence detected - return to command mode
            modemHangup();
            modemSend("OK");  // Escape successful
            return;
        }

        if (pppBuf == NULL) {
            pppBuf = (uint8_t *)malloc(4096);
            if (pppBuf == NULL) return;
            pppMaxBufSize = 4096;
            pppCurBufLen = 0;
        }

        if (c == PPP_FLAG) {
            if (pppCurBufLen > 0) {
                ppp_processFrame(pppBuf, pppCurBufLen);
                pppCurBufLen = 0;
                pppEscaped = false;
            }
        } else if (c == PPP_ESCAPE) {
            pppEscaped = true;
        } else {
            if (pppEscaped) {
                c ^= PPP_TRANS;
                pppEscaped = false;
            }

            if (pppCurBufLen < pppMaxBufSize)
                pppBuf[pppCurBufLen++] = c;
        }
    }
}

void setup() {
    Serial.begin(9600);  // Standard baud rate for modem compatibility
    delay(1000);


    // Load settings (persistent across reboots)
    preferences.begin("zs-dash", false);
    serialNumber = preferences.getUInt("serial", 0);
    if (serialNumber == 0) {
        // First boot - generate and save new serial
        serialNumber = esp_random() % 100000000;  // 0-99999999 (8 digits)
        preferences.putUInt("serial", serialNumber);
    } else {
    }
    // Load WiFi password and OBD filter
    String savedPass = preferences.getString("wifi_pass", "12345678");
    String savedFilter = preferences.getString("obd_filter", "OBD,ELM,V-LINK,IOS-");
    engineCylinders = preferences.getInt("eng_cyl", 6);
    tankCapacity = preferences.getFloat("tank_cap", 16.0);
    fixedMPG = preferences.getFloat("fixed_mpg", 23.0);
    // Infer displacement from cylinders
    if (engineCylinders == 4) engineDisplacement = 2.0;
    else if (engineCylinders == 8) engineDisplacement = 5.0;
    else engineDisplacement = 3.0;  // 6 cyl default
    strncpy(wifiPassword, savedPass.c_str(), sizeof(wifiPassword) - 1);
    strncpy(obdFilter, savedFilter.c_str(), sizeof(obdFilter) - 1);
    // Load MPG tracking data
    mapMPG = preferences.getFloat("map_mpg", 0);
    totalMiles = preferences.getFloat("total_miles", 0);
    lastFuelLevel = preferences.getFloat("last_fuel", 0);
    lastSavedMiles = totalMiles;  // Sync save tracker

    if (mapMPG > 0) {
    } else {
    }
    if (totalMiles > 0) {
    }
    preferences.end();
    snprintf(ssid, sizeof(ssid), "ZS-%08u", serialNumber);

    // Load modem phonebook
    loadPhonebook();

    // Initialize alarm system GPIO pins
    pinMode(PIN_STARTER, OUTPUT);
    pinMode(PIN_IGNITION, OUTPUT);
    pinMode(PIN_ACCESSORY, OUTPUT);
    pinMode(PIN_LOCK, OUTPUT);
    pinMode(PIN_UNLOCK, OUTPUT);
    pinMode(PIN_SIREN, OUTPUT);
    pinMode(PIN_TRIGGER, INPUT);

    // Set all relays to OFF state
    digitalWrite(PIN_STARTER, RELAY_OFF);
    digitalWrite(PIN_IGNITION, RELAY_OFF);
    digitalWrite(PIN_ACCESSORY, RELAY_OFF);
    digitalWrite(PIN_LOCK, RELAY_OFF);
    digitalWrite(PIN_UNLOCK, RELAY_OFF);
    digitalWrite(PIN_SIREN, RELAY_OFF);


    // Set up WiFi AP FIRST to ensure hotspot is always available
    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // Max WiFi power (19.5 dBm / ~90mW)
    WiFi.softAP(ssid, wifiPassword);

    // Enable WiFi/BLE coexistence - prefer WiFi to keep AP stable
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);


    server.on("/", handleRoot);
    server.on("/api/data", handleData);
    server.on("/api/dtc", handleDTC);
    server.on("/api/obd/status", handleOBDStatus);
    server.on("/api/obd/scan", handleOBDScan);
    server.on("/api/obd/connect", handleOBDConnect);
    server.on("/api/obd/disconnect", handleOBDDisconnect);
    server.on("/api/obd/settings", handleOBDSettings);
    server.on("/api/control/lock", handleLock);
    server.on("/api/control/unlock", handleUnlock);
    server.on("/api/control/domelight", handleDomeLight);
    server.on("/api/control/remotestart", handleRemoteStart);
    server.on("/api/control/status", handleStatus);
    server.on("/api/mpg/reset", handleMPGReset);
    server.begin();

    // Init BLE after WiFi is running
    char bleName[32];
    snprintf(bleName, sizeof(bleName), "ZS-Dash-%04X", serialNumber & 0xFFFF);
    NimBLEDevice::init(bleName);
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(200);  // Less aggressive to allow WiFi time
    pBLEScan->setWindow(150);

    // Try to connect to OBD
    if (connectToELM()) {
        obdConnected = true;
        initELM();
    } else {
    }

}

void sendHTMLChunked(const char* htmlStr) {
    // Send HTML in chunks to avoid serial buffer overflow
    const size_t len = HTML_LENGTH;  // Use hardcoded length (strlen stops at null bytes in base64 data)
    const size_t chunkSize = 512;

    for (size_t i = 0; i < len; i += chunkSize) {
        size_t remaining = len - i;
        size_t sendSize = (remaining < chunkSize) ? remaining : chunkSize;
        Serial.write((const uint8_t*)(htmlStr + i), sendSize);
        Serial.flush();
        delay(1);  // Small delay to prevent buffer overflow
    }
    Serial.println();  // Final newline
}

void loop() {
    server.handleClient();

    // Check for escape sequence completion when in online mode (even when no data)
    if (modemState == MODEM_ONLINE && !Serial.available()) {
        if (checkEscapeSequence(0)) {
            // Escape sequence detected - return to command mode
            modemHangup();
            modemSend("OK");  // Escape successful
        }
    }

    // Serial command parser
    if (Serial.available()) {
        // If in PPP mode, handle binary PPP data
        if (modemState == MODEM_ONLINE) {
            ppp_serialIncoming();
        } else {
            String cmd = Serial.readStringUntil('\n');
            cmd.trim();

            // AT modem commands take priority
            String cmdUpper = cmd;
            cmdUpper.toUpperCase();
            if (cmdUpper.startsWith("AT")) {
                processATCommand(cmd);
            } else if (cmd.startsWith("GET ")) {
            String path = cmd.substring(4);
            path.trim();

            // Remove query params for routing
            int queryPos = path.indexOf('?');
            String route = (queryPos > 0) ? path.substring(0, queryPos) : path;
            String params = (queryPos > 0) ? path.substring(queryPos + 1) : "";

            // Serve HTML page
            if (route == "/") {
                sendHTMLChunked(html);
            } else {
                String json = "";

                if (route == "/api/data") {
                json = buildDataJSON();
            } else if (route == "/api/dtc") {
                json = buildDTCJSON();
            } else if (route == "/api/obd/status") {
                json = buildOBDStatusJSON();
            } else if (route == "/api/obd/scan") {
                json = buildOBDScanJSON();
            } else if (route == "/api/obd/connect") {
                String addr = getQueryParam(params, "addr");
                json = buildOBDConnectJSON(addr);
            } else if (route == "/api/obd/disconnect") {
                json = buildOBDDisconnectJSON();
            } else if (route == "/api/obd/settings") {
                json = buildOBDSettingsJSON(params);
            } else if (route == "/api/control/lock") {
                json = buildLockJSON();
            } else if (route == "/api/control/unlock") {
                json = buildUnlockJSON();
            } else if (route == "/api/control/domelight") {
                json = buildDomeLightJSON(params);
            } else if (route == "/api/control/remotestart") {
                json = buildRemoteStartJSON();
            } else if (route == "/api/control/status") {
                json = buildStatusJSON();
            } else if (route == "/api/mpg/reset") {
                json = buildMPGResetJSON();
                } else {
                    json = "{\"error\":\"unknown route\"}";
                }

                Serial.println(json);
            }
            }
        }
    }


    // Reconnect if needed
    if (!obdConnected && millis() - lastOBDQuery > 15000) {
        if (connectToELM()) {
            obdConnected = true;
            initELM();
        }
        lastOBDQuery = millis();
    }

    // Query OBD
    if (obdConnected && millis() - lastOBDQuery > 200) {
        queryOBD();
        lastOBDQuery = millis();
        server.handleClient();  // Extra WiFi handling after OBD query
    }

    // MPG data is now saved immediately on fill detection (no periodic save needed)

    // Alarm system monitoring
    if (millis() - lastTriggerCheck > 100) {  // Check trigger every 100ms
        bool triggerState = digitalRead(PIN_TRIGGER);

        if (alarmArmed && triggerState && !alarmTriggered) {
            // Alarm triggered!
            alarmTriggered = true;
            digitalWrite(PIN_SIREN, RELAY_ON);
            sirenActive = true;
            sirenStartTime = millis();
        }

        lastTriggerCheck = millis();
    }

    // Auto-shutoff siren after 30 seconds (prevent battery drain)
    if (sirenActive && (millis() - sirenStartTime > 30000)) {
        digitalWrite(PIN_SIREN, RELAY_OFF);
        sirenActive = false;
    }

    // Monitor engine running state (if remote start active)
    if (remoteStartActive) {
        // Check if 10 minutes elapsed
        if (millis() - remoteStartTimer > REMOTE_START_DURATION) {
            digitalWrite(PIN_IGNITION, RELAY_OFF);
            digitalWrite(PIN_ACCESSORY, RELAY_OFF);
            engineRunning = false;
            remoteStartActive = false;
        }
        // Check if engine stalled (RPM dropped to 0)
        else if (rpm < 100) {
            digitalWrite(PIN_IGNITION, RELAY_OFF);
            digitalWrite(PIN_ACCESSORY, RELAY_OFF);
            engineRunning = false;
            remoteStartActive = false;
        }
    }

    // Tablet on-time logic monitoring
    if (tabletOnTimerActive) {
        // Check if 5 minutes elapsed
        if (millis() - tabletOnTimer > TABLET_ON_DURATION) {
            digitalWrite(PIN_ACCESSORY, RELAY_OFF);
            domeLightOn = false;
            tabletOnTimerActive = false;
        }
        // Check if trigger pin activated (person exited vehicle)
        else if (digitalRead(PIN_TRIGGER)) {
            digitalWrite(PIN_ACCESSORY, RELAY_OFF);
            domeLightOn = false;
            tabletOnTimerActive = false;
        }
    }

    yield();  // Give WiFi stack time
}
