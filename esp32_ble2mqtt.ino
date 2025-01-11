/*
    OH2MP ESP32 BLE2MQTT

    See https://github.com/oh2mp/esp32_ble2mqtt

*/

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include "FS.h"
#include <LITTLEFS.h>
#include <time.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#include "esp_mac.h"

#include <PubSubClient.h>

// #define CONFIG_LITTLEFS_SPIFFS_COMPAT 1

#define BUTTON 0                 // Push button for starting portal mode. On devkit this is BOOT button.
#define APTIMEOUT 120000         // Portal timeout. Reboot after ms if no activity.

// LED pins and channels
#define LED_R_PIN   21
#define LED_G_PIN   22
#define LED_B_PIN   23
#define LED_R   0
#define LED_G   1
#define LED_B   2

#define MAX_TAGS 16

// Tag type enumerations and names
#define TAG_RUUVI   1
#define TAG_MIJIA   2
#define TAG_ENERGY  3
#define TAG_WATER   4
#define TAG_THCPL   5
#define TAG_DS1820  6
#define TAG_DHT     7
#define TAG_WATTSON 8
#define TAG_MOPEKA  9
#define TAG_IBSTH2  10
#define TAG_ALPICOOL 11

const char type_name[12][10] PROGMEM = {"", "\u0550UUVi", "ATC_Mi", "Energy", "Water", "Flame", "DS18x20", "DHTxx", "Wattson", "Mopeka\u2713", "IBS-TH2", "Alpicool"};
// end of tag type enumerations and names

char tagdata[MAX_TAGS][32];      // space for raw tag data unparsed
char tagname[MAX_TAGS][24];      // tag names
char tagmac[MAX_TAGS][18];       // tag macs
int  tagrssi[MAX_TAGS];          // RSSI for each tag

uint8_t tagtype[MAX_TAGS];       // "cached" value for tag type
uint8_t tagcount = 0;            // total amount of known tags
int interval = 1;
time_t lastpublish = 0;
hw_timer_t *timer = NULL;        // for watchdog

char gattcache[32];              // Space for caching GATT payload
int task_counter = 0;
TaskHandle_t bletask = NULL;

// Default hostname base. Last 3 octets of MAC are added as hex.
// The hostname can be changed explicitly from the portal.
char myhostname[64] = "esp32-ble2mqtt-";

// placeholder values
char topicbase[256] = "sensors";
char mqtt_user[64] = "foo";
char mqtt_pass[64] = "bar";
char mqtt_host[64] = "192.168.36.99";
int  mqtt_port = 1883;

WiFiMulti WiFiMulti;
WiFiClient wificlient;
PubSubClient client(wificlient);

WebServer server(80);
IPAddress apIP(192, 168, 4, 1);                  // portal ip address
const char my_ssid[] PROGMEM = "ESP32 BLE2MQTT"; // AP SSID
uint32_t portal_timer = 0;
uint32_t ble_timer = 0;

uint8_t alpicool_index = 0xFF;  // If we have an Alpicool, store its tag index here.
bool alpicool_heard = false;    // Did we hear one on last iteration?
bool scanning = false;

char heardtags[MAX_TAGS][18];
uint8_t heardtagtype[MAX_TAGS];

File file;
BLEScan* blescan;
BLEScanResults* foundDevices;
BLEClient *pClient;
BLERemoteService *pRemoteService;
BLERemoteCharacteristic *rCharacteristic;
BLERemoteCharacteristic *wCharacteristic;

/* ------------------------------------------------------------------------------- */
/* Get known tag index from MAC address. Format: 12:34:56:78:9a:bc */
uint8_t getTagIndex(const char *mac) {
    for (uint8_t i = 0; i < MAX_TAGS; i++) {
        if (strcmp(tagmac[i], mac) == 0) {
            return i;
        }
    }
    return 0xFF; // no tag with this mac found
}

/* ------------------------------------------------------------------------------- */
/*  Detect tag type from payload and mac

    Ruuvi tags (Manufacturer ID 0x0499) with data format V5 only

    Homemade tags (Manufacturer ID 0x02E5 Espressif Inc)
     The sketches are identified by next two bytes after MFID.
     0xE948 water tank gauge https://github.com/oh2mp/esp32_watersensor/
     0x1A13 thermocouple sensor for gas flame https://github.com/oh2mp/esp32_max6675_beacon/
     0xACDC energy meter pulse counter

    Xiaomi Mijia thermometer with atc1441 custom firmware.
     https://github.com/atc1441/ATC_MiThermometer

*/

uint8_t tagTypeFromPayload(const uint8_t *payload, const uint8_t *mac) {
    // Has manufacturerdata? If so, check if this is known type.
    if (memcmp(payload, "\x02\x01\x06", 3) == 0 && payload[4] == 0xFF) {
        if (memcmp(payload + 5, "\x99\x04\x05", 3) == 0)      return TAG_RUUVI;
        if (memcmp(payload + 5, "\xE5\x02\xDC\xAC", 4) == 0)  return TAG_ENERGY;
        if (memcmp(payload + 5, "\xE5\x02\x48\xE9", 4) == 0)  return TAG_WATER;
        if (memcmp(payload + 5, "\xE5\x02\x13\x1A", 4) == 0)  return TAG_THCPL;
        if (memcmp(payload + 5, "\xE5\x02\x20\x18", 4) == 0)  return TAG_DS1820;
    }
    // Alpicool fridge?
    if (memcmp(payload, "\x02\x01\x06", 3) == 0 && memcmp(payload + 9, "ZHJIELI", 7) == 0) return TAG_ALPICOOL;

    // ATC_MiThermometer? The data should contain 10161a18 in the beginning and mac at offset 4.
    if (memcmp(payload, "\x10\x16\x1A\x18", 4) == 0 && memcmp(mac, payload + 4, 6) == 0) return TAG_MIJIA;
    // Mopeka gas tank sensor?
    if (memcmp(payload, "\x1A\xFF\x0D\x00", 4) == 0 && payload[26] == mac[5]) return TAG_MOPEKA;
    
    return 0xFF; // unknown
}

/* ------------------------------------------------------------------------------- */
/*  Known devices callback
    /* ------------------------------------------------------------------------------- */

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
        void onResult(BLEAdvertisedDevice advDev) {
            set_led(0, 0, 128);
            uint8_t payload[32];
            uint8_t taginx = getTagIndex(advDev.getAddress().toString().c_str());

            // we are interested about known and saved BLE devices only
            if (taginx == 0xFF || tagname[taginx][0] == 0) return;

            memset(payload, 0, 32);
            memcpy(payload, advDev.getPayload(), 32);
            memset(tagdata[taginx], 0, sizeof(tagdata[taginx]));

            // ignore if payload doesn't contain valid data.
            if (memcmp(payload+7, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 24) == 0) {
                return;
            }
            // Don't we know the type of this device yet?
            if (tagtype[taginx] == 0) {
                uint8_t mac[6];
                tagtype[taginx] = tagTypeFromPayload(payload, mac);
            }
            // Inkbird IBS-TH2 or Alpicool?
            if (tagtype[taginx] == 0xFF) {
                if (advDev.haveServiceUUID()) {
                    if (strcmp(advDev.getServiceUUID().toString().c_str(), "0000fff0-0000-1000-8000-00805f9b34fb") == 0) {
                        tagtype[taginx] = TAG_IBSTH2;
                    }
                }
                if (memcmp(payload, "\x02\x01\x06", 3) == 0 && memcmp(payload + 9, "ZHJIELI", 7) == 0) {
                    tagtype[taginx] = TAG_ALPICOOL;
                }
            }

            // Copy the payload to tagdata
            memcpy(tagdata[taginx], payload, 32);
            if (tagtype[taginx] == TAG_ALPICOOL) {
                memcpy(tagdata[taginx], gattcache, 32);
                alpicool_index = taginx;
                alpicool_heard = true;
            }

            tagrssi[taginx] = advDev.getRSSI();

            Serial.printf("BLE callback: payload=");
            for (uint8_t i = 0; i < 32; i++) {
                Serial.printf("%02x", payload[i]);
            }
            Serial.printf("; ID=%d; type=%d; addr=%s; name=%s\n", taginx, tagtype[taginx], tagmac[taginx], tagname[taginx]);
            set_led(0, 0, 0);
        }
};

/* ------------------------------------------------------------------------------- */
/*  Alpicool callback
/* ------------------------------------------------------------------------------- */
void alpicoolCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {

    memset(gattcache,0,sizeof(gattcache));
    memcpy(gattcache,pData,length);

    short temperature = 0;
    short wanted = 0;

    temperature = (short)pData[18];
    wanted = (short)pData[8];
    Serial.print("Alpicool GATT payload=");
    for (uint8_t i = 0; i < 32; i++) {
        Serial.printf("%02x", pData[i]);
    }
    Serial.println("");
    // Client must disconnect or otherwise eg. mobile app can't connect.
    // These fridges can handle only one connection at a time.
    pClient->disconnect();
}

/* ------------------------------------------------------------------------------- */
/*  Find new devices when portal is started
    /* ------------------------------------------------------------------------------- */
class ScannedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
        void onResult(BLEAdvertisedDevice advDev) {
            set_led(0, 0, 128);
            uint8_t payload[32];
            uint8_t taginx = getTagIndex(advDev.getAddress().toString().c_str());

            Serial.printf("Heard %s %s\nPayload: ", advDev.toString().c_str(), advDev.getName().c_str());

            memcpy(payload, advDev.getPayload(), 32);
            for (uint8_t i = 0; i < 32; i++) {
                Serial.printf("%02x", payload[i]);
            }
            Serial.printf("\n");

            // skip known tags, we are trying to find new
            if (taginx != 0xFF) return;

            /*  we are interested only about Ruuvi tags (Manufacturer ID 0x0499)
                and self made tags that have Espressif ID 0x02E5
                and Xiaomi Mijia thermometer with atc1441 custom firmware
            */
            uint8_t mac[6];
            memcpy(mac, advDev.getAddress().getNative(), 6);
            uint8_t htype = tagTypeFromPayload(payload, mac);

            // Check if this is Inkbird IBS-TH2
            if (htype == 0xFF) {
                if (advDev.haveServiceUUID()) {
                    if (strcmp(advDev.getServiceUUID().toString().c_str(), "0000fff0-0000-1000-8000-00805f9b34fb") == 0) {
                        htype = TAG_IBSTH2;
                    }
                }
            }

            if (htype != 0xFF && htype != 0) {
                for (uint8_t i = 0; i < MAX_TAGS; i++) {
                    if (strlen(heardtags[i]) == 0) {
                        strcpy(heardtags[i], advDev.getAddress().toString().c_str());
                        heardtagtype[i] = htype;
                        Serial.printf("Heard new tag: %s %s\n", heardtags[i], type_name[htype]);
                        break;
                    }
                }
            } else {
                Serial.print("Ignoring unsupported device.\n");
            }
            set_led(0, 0, 0);
        }
};

/* ------------------------------------------------------------------------------- */
void loadWifis() {
    if (LITTLEFS.exists("/littlefs/known_wifis.txt")) {
        char ssid[33];
        char pass[65];

        file = LITTLEFS.open("/littlefs/known_wifis.txt", "r", false);
        while (file.available()) {
            memset(ssid, '\0', sizeof(ssid));
            memset(pass, '\0', sizeof(pass));
            file.readBytesUntil('\t', ssid, 32);
            file.readBytesUntil('\n', pass, 64);
            WiFiMulti.addAP(ssid, pass);
            Serial.printf("wifi loaded: %s / %s\n", ssid, pass);
        }
        file.close();
    }
    if (LITTLEFS.exists("/littlefs/myhostname.txt")) {
        file = LITTLEFS.open("/littlefs/myhostname.txt", "r", false);
        memset(myhostname, 0, sizeof(myhostname));
        file.readBytesUntil('\n', myhostname, sizeof(myhostname));
        file.close();
    }
    Serial.printf("My hostname: %s\n", myhostname);
}
/* ------------------------------------------------------------------------------- */
void loadSavedTags() {
    char sname[25];
    char smac[18];
    for (uint8_t i = 0; i < MAX_TAGS; i++) {
        tagtype[i] = 0;
        memset(tagname[i], 0, sizeof(tagname[i]));
        memset(tagdata[i], 0, sizeof(tagdata[i]));
    }

    if (LITTLEFS.exists("/littlefs/known_tags.txt")) {
        uint8_t foo = 0;
        file = LITTLEFS.open("/littlefs/known_tags.txt", "r", false);
        while (file.available()) {
            memset(sname, '\0', sizeof(sname));
            memset(smac, '\0', sizeof(smac));

            file.readBytesUntil('\t', smac, 18);
            file.readBytesUntil('\n', sname, 25);
            while (isspace(smac[strlen(smac) - 1]) && strlen(smac) > 0) smac[strlen(smac) - 1] = 0;
            while (isspace(sname[strlen(sname) - 1]) && strlen(sname) > 0) sname[strlen(sname) - 1] = 0;
            if (sname[strlen(sname) - 1] == 13) sname[strlen(sname) - 1] = 0;
            strcpy(tagmac[foo], smac);
            strcpy(tagname[foo], sname);
            foo++;
            if (foo >= MAX_TAGS) break;
            tagcount++;
        }
        file.close();
    }
}
/* ------------------------------------------------------------------------------- */
void loadMQTT() {
    if (LITTLEFS.exists("/littlefs/mqtt.txt")) {
        char tmpstr[8];
        memset(tmpstr, 0, sizeof(tmpstr));
        memset(mqtt_host, 0, sizeof(mqtt_host));
        memset(mqtt_user, 0, sizeof(mqtt_user));
        memset(mqtt_pass, 0, sizeof(mqtt_pass));
        memset(topicbase, 0, sizeof(topicbase));

        file = LITTLEFS.open("/littlefs/mqtt.txt", "r", false);
        while (file.available()) {
            file.readBytesUntil(':', mqtt_host, sizeof(mqtt_host));
            file.readBytesUntil('\n', tmpstr, sizeof(tmpstr));
            mqtt_port = atoi(tmpstr);
            memset(tmpstr, 0, sizeof(tmpstr));
            if (mqtt_port < 1 || mqtt_port > 65535) mqtt_port = 1883; // default
            file.readBytesUntil(':', mqtt_user, sizeof(mqtt_user));
            file.readBytesUntil('\n', mqtt_pass, sizeof(mqtt_pass));
            file.readBytesUntil('\n', topicbase, sizeof(topicbase));
            file.readBytesUntil('\n', tmpstr, sizeof(tmpstr));
            interval = atoi(tmpstr);
        }
        file.close();
        Serial.printf("MQTT broker: %s:%d - topic prefix: %s\n", mqtt_host, mqtt_port, topicbase);
    }
}
/* ------------------------------------------------------------------------------- */
void set_led(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(LED_R, r);
    ledcWrite(LED_G, g);
    ledcWrite(LED_B, b);
}
/* ------------------------------------------------------------------------------- */
// Color effect for boot
void led_fx() {
    int r = 128; int g = 0; int b = 0;
    for (int i = 0; i < 3000; i++) {
        if (r > 0 && b == 0) {
            r--;
            g++;
        }
        if (g > 0 && r == 0) {
            g--;
            b++;
        }
        if (b > 0 && g == 0) {
            r++;
            b--;
        }
        r = constrain(r, 0, 128); g = constrain(g, 0, 128); b = constrain(b, 0, 128);
        set_led(r, g, b);
        delay(1);
    }
}
/* ------------------------------------------------------------------------------- */
// This happens if watchdog timer is triggered. See the end of the setup() function.
void IRAM_ATTR reset_esp32() {
    ets_printf("Alarm. Reboot\n");
    esp_restart();
}

/* ------------------------------------------------------------------------------- */
void setup() {
    Serial.begin(115200);
    Serial.println("\n\nESP32 BLE2MQTT");

    pinMode(BUTTON, INPUT_PULLUP);

    // Reset real time clock
    timeval epoch = {0, 0};
    const timeval *tv = &epoch;
    settimeofday(tv, NULL);

    // Prepare watchdog
    timer = timerBegin(333333);
    timerAttachInterrupt(timer, &reset_esp32);
    if (interval > 0) {
        timerAlarm(timer, interval * 180E+6 + 15E+6, false , 0); // set time to 3x interval (µs) +15s
    } else {
        timerAlarm(timer, 195E+6, false, 0);                  // if interval < 1, set it to 3m 15s
    }

    // Append last 3 octets of MAC to the default hostname
    uint8_t mymac[6];
    esp_read_mac(mymac, ESP_MAC_WIFI_STA); 
    char mac_end[8];
    sprintf(mac_end, "%02x%02x%02x", mymac[3], mymac[4], mymac[5]);
    strcat(myhostname, mac_end);

    ledcAttachChannel(LED_R_PIN, 5000, 8, LED_R);
    ledcAttachChannel(LED_G_PIN, 5000, 8, LED_G);
    ledcAttachChannel(LED_B_PIN, 5000, 8, LED_B);
    led_fx();
    set_led(0, 0, 0);

    for (uint8_t i = 0; i < MAX_TAGS; i++) {
        memset(tagname[i], 0, sizeof(tagname[i]));
        memset(tagdata[i], 0, sizeof(tagdata[i]));
        memset(tagmac[i], 0, sizeof(tagmac[i]));
        tagrssi[i] = 0;
        tagtype[i] = 0;
    }

    LITTLEFS.begin(false, "/littlefs", 1);
    loadSavedTags();
    loadMQTT();
    memset(gattcache,0,sizeof(gattcache));

    BLEDevice::init("");
    blescan = BLEDevice::getScan();
    pClient = BLEDevice::createClient();

    if (tagcount == 0) {
        startPortal();
    } else {

        blescan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
        blescan->setActiveScan(true);
        blescan->setInterval(100);
        blescan->setWindow(99);

        loadWifis();
        client.setServer(mqtt_host, mqtt_port);

        // https://github.com/espressif/arduino-esp32/issues/2537#issuecomment-508558849
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
        WiFi.setHostname(myhostname);

        xTaskCreate(ble_task, "bletask", 4096, NULL, 1, &bletask);
    }
}

/* ------------------------------------------------------------------------------- */
void loop() {
    uint8_t do_send = 0;

    if (portal_timer == 0) {
        if (time(NULL) == 0 || interval == 0) do_send = 1;
        if (interval > 0) {
            if ((time(NULL) - lastpublish) >= (interval * 60)) do_send = 1;
        }
        if (digitalRead(BUTTON) == LOW) {
            do_send = 0;
            startPortal();
        }
        // Sometimes GATT client connecting hangs and in the library the timeout is something like 50 days. 
        // We don't want to wait that long.
        if (millis() - ble_timer > 60000) {
            Serial.println("BLE looks to be hanged. Reboot.");
            ESP.restart();
        }
    }
    if (do_send == 1) {
        while (scanning) delay(100);
        mqtt_send();
    }

    if (portal_timer > 0) {     // are we in portal mode?
        if (millis() % 500 < 250) {
            set_led(int((millis() - portal_timer) / (APTIMEOUT / 128)), 128 - int((millis() - portal_timer) / (APTIMEOUT / 64) * 2), 0);
        } else {
            set_led(0, 0, 0);
        }
        server.handleClient();
        if (millis() - portal_timer > APTIMEOUT) {
            Serial.println("Portal timeout. Booting.");
            delay(1000);
            ESP.restart();
        }
    }
}

/* ------------------------------------------------------------------------------- */

void mqtt_send() {
    char json[512];
    char topic[512];
    short temperature = 0;
    unsigned short humidity;
    unsigned short foo;
    int pressure;
    int voltage;
    uint32_t wh;
    uint32_t wht;
    boolean published = false;

    WiFi.mode(WIFI_STA);

    for (uint8_t curr_tag = 0; curr_tag < MAX_TAGS; curr_tag++) {
        if (strlen(tagname[curr_tag]) > 0 && tagdata[curr_tag][0] != 0) {
            // Ruuvi tags
            if (tagtype[curr_tag] == TAG_RUUVI) {
                if (tagdata[curr_tag][0] != 0) {
                    temperature = ((short)tagdata[curr_tag][8] << 8) | (unsigned short)tagdata[curr_tag][9];
                    humidity = ((unsigned short)tagdata[curr_tag][10] << 8) | (unsigned short)tagdata[curr_tag][11];
                    foo = ((unsigned short)tagdata[curr_tag][20] << 8) + (unsigned short)tagdata[curr_tag][21];
                    voltage = ((double)foo / 32  + 1600);
                    pressure = ((unsigned short)tagdata[curr_tag][12] << 8) + (unsigned short)tagdata[curr_tag][13] + 50000;

                    sprintf(json, "{\"type\":%d,\"t\":%d,\"rh\":%d,\"bu\":%d,\"ap\":%d,\"s\":%d}",
                            tagtype[curr_tag], int(temperature * .05), int((float)humidity * .0025),
                            voltage, int(pressure / 100), abs(tagrssi[curr_tag]));
                }
            }
            // Other tags --------------------------------------------------------------------------------------
            // water gauge
            if (tagtype[curr_tag] == TAG_WATER) {
                if (tagdata[curr_tag][0] != 0) {
                    sprintf(json, "{\"type\":%d,\"lv\":%d,\"s\":%d}",
                            tagtype[curr_tag], (unsigned int)tagdata[curr_tag][10], abs(tagrssi[curr_tag]));
                }
            }
            // flame thermocouple
            if (tagtype[curr_tag] == TAG_THCPL) {
                // get the temperature
                foo = (((unsigned short)tagdata[curr_tag][10] << 8) + (unsigned short)tagdata[curr_tag][9]) >> 2;
                sprintf(json, "{\"type\":%d,\"t\":%d,\"s\":%d}", tagtype[curr_tag], foo * 10, abs(tagrssi[curr_tag]));
            }
            // energy meter pulse counter
            if (tagtype[curr_tag] == TAG_ENERGY) {
                if (tagdata[curr_tag][0] != 0) {
                    wh = (((uint32_t)tagdata[curr_tag][16] << 24) + ((uint32_t)tagdata[curr_tag][15] << 16)
                          + ((uint32_t)tagdata[curr_tag][14] << 8) + (uint32_t)tagdata[curr_tag][13]);

                    wht = (((uint32_t)tagdata[curr_tag][12] << 24) + ((uint32_t)tagdata[curr_tag][11] << 16)
                           + ((uint32_t)tagdata[curr_tag][10] << 8) + (uint32_t)tagdata[curr_tag][9]);

                    sprintf(json, "{\"type\":%d,\"e\":%d,\"et\":%d,\"s\":%d}", tagtype[curr_tag], wh, wht, abs(tagrssi[curr_tag]));
                }
            }
            // Xiaomi Mijia thermometer with ATC_MiThermometer custom firmware by atc1441
            if (tagtype[curr_tag] == TAG_MIJIA) {
                if (tagdata[curr_tag][0] != 0) {
                    temperature = ((short)tagdata[curr_tag][10] << 8) | (unsigned short)tagdata[curr_tag][11];
                    humidity = (unsigned short)tagdata[curr_tag][12];
                    voltage = ((short)tagdata[curr_tag][14] << 8) | (unsigned short)tagdata[curr_tag][15];
                    sprintf(json, "{\"type\":%d,\"t\":%d,\"rh\":%d,\"bu\":%d,\"s\":%d}",
                            tagtype[curr_tag], int(temperature), int(humidity), int(voltage), abs(tagrssi[curr_tag]));
                }
            }
            // esp32 + ds1820 beacon
            if (tagtype[curr_tag] == TAG_DS1820) {
                if (tagdata[curr_tag][0] != 0) {
                    temperature = ((short)tagdata[curr_tag][10] << 8) | (unsigned short)tagdata[curr_tag][9];
                    sprintf(json, "{\"type\":%d,\"t\":%d,\"s\":%d}",
                            tagtype[curr_tag], int(temperature), abs(tagrssi[curr_tag]));
                }
            }
            // Mopeka gas tank sensor
            if (tagtype[curr_tag] == TAG_MOPEKA) {
                // This algorithm has been got from Mopeka Products, LLC.
                uint8_t level = 0x35;
                for (uint8_t i = 8; i < 27; i++) {
                    level ^= tagdata[curr_tag][i];
                }
                voltage = int(((float)tagdata[curr_tag][6] / 256.0f * 2.0f + 1.5f)*1000); // Mopeka specification
                sprintf(json, "{\"type\":%d,\"gh\":%d,\"s\":%d,\"bu\":%d}",
                        tagtype[curr_tag], int(level*.762), abs(tagrssi[curr_tag]),voltage);
            }
            // Inkbird IBS-TH2
            if (tagtype[curr_tag] == TAG_IBSTH2) {
                if (tagdata[curr_tag][0] != 0) {
                    temperature = ((short)tagdata[curr_tag][15] << 8) | (unsigned short)tagdata[curr_tag][14];
                    temperature = round(temperature/10);
                    voltage = (short)tagdata[curr_tag][21]; // in Inkbird this is percentage, not voltage.
                    sprintf(json, "{\"type\":%d,\"t\":%d,\"bp\":%d,\"s\":%d}",
                            tagtype[curr_tag], int(temperature), voltage, abs(tagrssi[curr_tag]));
                    if (memcmp(tagdata[curr_tag]+7, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 25) == 0) {
                       json[0] = 0; // no valid data got, so set this tag to be skipped
                    }
                }
            }
            if (tagtype[curr_tag] == TAG_ALPICOOL && alpicool_heard) {
                if (tagdata[curr_tag][0] != 0) {
                    temperature = (short)tagdata[curr_tag][18] * 10;
                    voltage = int((float)tagdata[curr_tag][20] * 1000 + (float)tagdata[curr_tag][21] * 100);
                    sprintf(json, "{\"type\":%d,\"t\":%d,\"tt\":%d,\"bu\":%d,\"s\":%d}",
                            tagtype[curr_tag], int(temperature), (short)tagdata[curr_tag][8] * 10, voltage, abs(tagrssi[curr_tag]));  
                }
            }

            if (json[0] != 0) {
                memset(topic, 0, sizeof(topic));
                sprintf(topic, "%s/%s", topicbase, tagname[curr_tag]);

                // convert possible spaces to underscores in topic
                for (uint8_t i = 0; i < strlen(topic); i++) {
                    if (topic[i] == 32) topic[i] = '_';
                }

                if (WiFiMulti.run() == WL_CONNECTED) {
                    if (curr_tag == 0) {
                        Serial.printf("Connected to SSID=%s - My IP=%s\n",
                                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
                        Serial.flush();
                    }
                    if (client.connect(myhostname, mqtt_user, mqtt_pass)) {
                        if (client.publish(topic, json)) {
                            set_led(0, 128, 0);
                            timerWrite(timer, 0); //reset timer (feed watchdog)
                            Serial.printf("%s %s\n", topic, json);
                            Serial.flush();
                            memset(tagdata[curr_tag], 0, sizeof(tagdata[curr_tag]));
                            lastpublish = time(NULL);
                            published = true;
                        } else {
                            set_led(51, 0, 0);
                            Serial.print("Failed to publish MQTT, rc=");
                            Serial.println(client.state());
                        }
                    } else {
                        set_led(51, 0, 0);
                        Serial.printf("Failed to connect MQTT broker, state=%d\n", client.state());
                    }
                } else {
                    set_led(128, 0, 0);
                    Serial.printf("Failed to connect WiFi, status=%d\n", WiFi.status());
                }
            }
        }
        memset(json, 0, sizeof(json));
        delay(100);
    }
    ble_timer = millis(); // prevent to get false timeout if sending MQTT took long time.
    // If we published something, disconnect the client here to clean session.
    if (published) {
        client.disconnect();
        Serial.println("Sending MQTT complete");
    }
    set_led(0, 0, 0);
}

/* ------------------------------------------------------------------------------- */
/*
    This task handles BLE scanning and possible GATT request to an Alpicool fridge
*/
void ble_task(void *parameter) {

  task_counter = 0;

  while (1) {
      ble_timer = millis();
      alpicool_heard = false;

      Serial.printf("============= start scan at %d seconds\n", int(millis()/1000));
      scanning = true;
      foundDevices = blescan->start(11, false);
      blescan->clearResults();
      /*  Something is wrong if zero known tags is heard, so then reboot.
          Possible if all of them are out of range too, but that should not happen anyway.
      */
      if (foundDevices->getCount() == 0 && tagcount > 0) ESP.restart();
      Serial.printf("============= end scan\n");
  
      if (alpicool_index != 0xFF) {
          if (alpicool_heard) {
              if (!pClient->isConnected()) {
                  Serial.println("Connecting to Alpicool fridge");
                  pClient->connect(BLEAddress(tagmac[alpicool_index]));
                  pClient->setMTU(32);
                  pRemoteService = pClient->getService("00001234-0000-1000-8000-00805F9B34FB");
                  rCharacteristic = pRemoteService->getCharacteristic("00001236-0000-1000-8000-00805F9B34FB");
                  rCharacteristic->registerForNotify(alpicoolCallback);
                  wCharacteristic = pRemoteService->getCharacteristic("00001235-0000-1000-8000-00805F9B34FB");
              } else {
                  Serial.println("Was already connected. Disconnect.");
                  pClient->disconnect();
                  wCharacteristic = nullptr;
              }
          }
          // Send query request
          // See: https://github.com/klightspeed/BrassMonkeyFridgeMonitor
          if (wCharacteristic != nullptr && alpicool_heard) {
              Serial.println("Sending query to Alpicool fridge: fefe03010200");
              wCharacteristic->writeValue({0xfe, 0xfe, 3, 1, 2, 0}, 6);
          }
          vTaskDelay(1000 / portTICK_PERIOD_MS); // give one second
          pClient->disconnect();
      }
      scanning = false;
      Serial.printf("Task iteration: %d, Free heap: %d\n", task_counter++, ESP.getFreeHeap());

      vTaskDelay(30000 / portTICK_PERIOD_MS);
      yield();
  }  
}

/* ------------------------------------------------------------------------------- */
/*  Portal code begins here

     Yeah, I know that String objects are pure evil 😈, but this is meant to be
     rebooted immediately after saving all parameters, so it is quite likely that
     the heap will not fragmentate yet.
*/
/* ------------------------------------------------------------------------------- */

void startPortal() {

    Serial.print("Starting portal...");
    portal_timer = millis();
    timerWrite(timer, 0);
    if (bletask != nullptr) vTaskDelete(bletask);
    if (pClient->isConnected()) pClient->disconnect();

    for (uint8_t i = 0; i < MAX_TAGS; i++) {
        memset(heardtags[i], 0, sizeof(heardtags[i]));
    }
    Serial.print("\nListening 11 seconds for new tags...\n");

    // First listen 11 seconds to find new tags.
    set_led(0, 128, 128);
    blescan->setAdvertisedDeviceCallbacks(new ScannedDeviceCallbacks());
    blescan->setActiveScan(true);
    blescan->setInterval(100);
    blescan->setWindow(99);
    BLEScanResults* foundDevices = blescan->start(11, false);
    blescan->stop();
    blescan->clearResults();
    blescan = NULL;
    BLEDevice::deinit(true);
    set_led(0, 0, 0);

    portal_timer = millis();
    timerWrite(timer, 0);

    WiFi.disconnect();
    delay(100);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(my_ssid);
    delay(2000);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

    server.on("/", httpRoot);
    server.on("/style.css", httpStyle);
    server.on("/sensors.html", httpSensors);
    server.on("/savesens", httpSaveSensors);
    server.on("/wifis.html", httpWifi);
    server.on("/savewifi", httpSaveWifi);
    server.on("/mqtt.html", httpMQTT);
    server.on("/savemqtt", httpSaveMQTT);
    server.on("/boot", httpBoot);

    server.onNotFound([]() {
        server.sendHeader("Refresh", "1;url=/");
        server.send(404, "text/plain", "QSD QSY");
    });
    server.begin();
    Serial.println("Portal running.");
}
/* ------------------------------------------------------------------------------- */

void httpRoot() {
    portal_timer = millis();
    timerWrite(timer, 0);
    String html;

    file = LITTLEFS.open("/littlefs/index.html", "r", false);
    html = file.readString();
    file.close();

    server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */

void httpWifi() {
    String html;
    char tablerows[1024];
    char rowbuf[256];
    char ssid[33];
    char pass[64];
    int counter = 0;

    portal_timer = millis();
    timerWrite(timer, 0);

    memset(tablerows, '\0', sizeof(tablerows));

    file = LITTLEFS.open("/littlefs/wifis.html", "r", false);
    html = file.readString();
    file.close();

    if (LITTLEFS.exists("/littlefs/known_wifis.txt")) {
        file = LITTLEFS.open("/littlefs/known_wifis.txt", "r", false);
        while (file.available()) {
            memset(rowbuf, '\0', sizeof(rowbuf));
            memset(ssid, '\0', sizeof(ssid));
            memset(pass, '\0', sizeof(pass));
            file.readBytesUntil('\t', ssid, 33);
            file.readBytesUntil('\n', pass, 33);
            sprintf(rowbuf, "<tr><td>SSID</td><td><input type=\"text\" name=\"ssid%d\" maxlength=\"32\" value=\"%s\"></td></tr>", counter, ssid);
            strcat(tablerows, rowbuf);
            sprintf(rowbuf, "<tr><td>PASS</td><td><input type=\"text\" name=\"pass%d\" maxlength=\"63\" value=\"%s\"></td></tr>", counter, pass);
            strcat(tablerows, rowbuf);
            counter++;
        }
        file.close();
    }
    if (LITTLEFS.exists("/littlefs/myhostname.txt")) {
        file = LITTLEFS.open("/littlefs/myhostname.txt", "r", false);
        memset(myhostname, '\0', sizeof(myhostname));
        file.readBytesUntil('\n', myhostname, sizeof(myhostname));
        file.close();
    }

    html.replace("###TABLEROWS###", tablerows);
    html.replace("###COUNTER###", String(counter));
    html.replace("###MYHOSTNAME###", String(myhostname));

    if (counter > 3) {
        html.replace("table-row", "none");
    }

    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveWifi() {
    portal_timer = millis();
    timerWrite(timer, 0);
    String html;

    file = LITTLEFS.open("/littlefs/known_wifis.txt", "w");
    if (!file) {
        Serial.println("Failed to open file for writing");
    }

    for (int i = 0; i < server.arg("counter").toInt(); i++) {
        if (server.arg("ssid" + String(i)).length() > 0) {
            file.print(server.arg("ssid" + String(i)));
            file.print("\t");
            file.print(server.arg("pass" + String(i)));
            file.print("\n");
        }
    }
    // Add new
    if (server.arg("ssid").length() > 0) {
        file.print(server.arg("ssid"));
        file.print("\t");
        file.print(server.arg("pass"));
        file.print("\n");
    }
    file.close();

    if (server.arg("myhostname").length() > 0) {
        file = LITTLEFS.open("/littlefs/myhostname.txt", "w");
        file.print(server.arg("myhostname"));
        file.print("\n");
        file.close();
    }

    file = LITTLEFS.open("/littlefs/ok.html", "r", false);
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "2;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpMQTT() {
    portal_timer = millis();
    timerWrite(timer, 0);
    String html;

    file = LITTLEFS.open("/littlefs/mqtt.html", "r", false);
    html = file.readString();
    file.close();

    html.replace("###HOSTPORT###", String(mqtt_host) + ":" + String(mqtt_port));
    html.replace("###USERPASS###", String(mqtt_user) + ":" + String(mqtt_pass));
    html.replace("###TOPICBASE###", String(topicbase));
    html.replace("###INTERVAL###", String(interval));

    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */
void httpSaveMQTT() {
    portal_timer = millis();
    timerWrite(timer, 0);
    String html;

    file = LITTLEFS.open("/littlefs/mqtt.txt", "w");
    file.printf("%s\n", server.arg("hostport").c_str());
    file.printf("%s\n", server.arg("userpass").c_str());
    file.printf("%s\n", server.arg("topicbase").c_str());
    file.printf("%s\n", server.arg("interval").c_str());
    file.close();
    loadMQTT(); // reread

    file = LITTLEFS.open("/littlefs/ok.html", "r", false);
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "2;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSensors() {
    String html;
    String tablerows; //char tablerows[16384];
    char rowbuf[1024];
    int counter = 0;

    portal_timer = millis();
    timerWrite(timer, 0);

    file = LITTLEFS.open("/littlefs/sensors.html", "r", false);
    html = file.readString();
    file.close();

    loadSavedTags();

    for (int i = 0 ; i < MAX_TAGS; i++) {
        if (strlen(tagmac[i]) == 0) continue;

        sprintf(rowbuf, "<tr><td colspan=\"2\" id=\"mac%d\">%s</td></tr><tr>\n",
                counter, tagmac[i]);
        tablerows += String(rowbuf);
        sprintf(rowbuf, "<tr><td><input type=\"text\" id=\"sname%d\" name=\"sname%d\" maxlength=\"24\" value=\"%s\">",
                counter, counter, tagname[i]);
        tablerows += String(rowbuf);
        sprintf(rowbuf, "<input type=\"hidden\" id=\"saddr%d\" name=\"saddr%d\" value=\"%s\">", counter, counter, tagmac[i]);
        tablerows += String(rowbuf);
        if (counter > 0) {
            sprintf(rowbuf, "<td><a onclick=\"moveup(%d)\">\u2191</a></td></tr>\n", counter);
            tablerows += String(rowbuf);
        } else {
            tablerows += "<td></td></tr>\n";
        }
        counter++;
    }
    if (strlen(heardtags[0]) != 0 && counter < MAX_TAGS) {
        for (int i = 0; i < MAX_TAGS; i++) {
            if (strlen(heardtags[i]) == 0) continue;
            if (getTagIndex(heardtags[i]) != 0xFF) continue;

            sprintf(rowbuf, "<tr><td colspan=\"2\" id=\"mac%d\">%s &nbsp; %s</td></tr>\n",
                    counter, heardtags[i], type_name[heardtagtype[i]]);
            tablerows += String(rowbuf);
            sprintf(rowbuf, "<tr><td><input type=\"text\" id=\"sname%d\" name=\"sname%d\" maxlength=\"24\">", counter, counter);
            tablerows += String(rowbuf);
            sprintf(rowbuf, "<input type=\"hidden\" id=\"saddr%d\" name=\"saddr%d\" value=\"%s\">",
                    counter, counter, heardtags[i]);
            tablerows += String(rowbuf);
            if (counter > 0) {
                sprintf(rowbuf, "<td><a onclick=\"moveup(%d)\">\u2191</a></td></tr>\n", counter);
                tablerows += String(rowbuf);
            } else {
                tablerows += "<td></td></tr>\n";
            }
            counter++;
            if (counter > MAX_TAGS) break;
        }
    }

    html.replace("###TABLEROWS###", tablerows);
    html.replace("###COUNTER###", String(counter));

    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveSensors() {
    portal_timer = millis();
    timerWrite(timer, 0);
    String html;

    file = LITTLEFS.open("/littlefs/known_tags.txt", "w");

    for (int i = 0; i < server.arg("counter").toInt(); i++) {
        if (server.arg("sname" + String(i)).length() > 0) {
            file.print(server.arg("saddr" + String(i)));
            file.print("\t");
            file.print(server.arg("sname" + String(i)));
            file.print("\n");
        }
    }
    file.close();
    loadSavedTags(); // reread

    file = LITTLEFS.open("/littlefs/ok.html", "r", false);
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "2;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpStyle() {
    portal_timer = millis();
    timerWrite(timer, 0);
    String css;

    file = LITTLEFS.open("/littlefs/style.css", "r", false);
    css = file.readString();
    file.close();
    server.send(200, "text/css", css);
}

/* ------------------------------------------------------------------------------- */
void httpBoot() {
    portal_timer = millis();
    timerWrite(timer, 0);
    String html;

    file = LITTLEFS.open("/littlefs/ok.html", "r", false);
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "2;url=about:blank");
    server.send(200, "text/html; charset=UTF-8", html);
    delay(1000);

    ESP.restart();
}
/* ------------------------------------------------------------------------------- */
