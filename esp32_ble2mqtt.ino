/*
 * OH2MP ESP32 BLE2MQTT
 *
 * See https://github.com/oh2mp/esp32_ble2mqtt
 *
 */

#include <FreeRTOS.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <time.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#include <PubSubClient.h>

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
#define TAG_RUUVI  1
#define TAG_MIJIA  2
#define TAG_ENERGY 3
#define TAG_WATER  4
#define TAG_THCPL  5

const char type_name[6][8] PROGMEM = {"", "\u0550UUVi", "ATC_Mi", "Energy", "Water", "TCouple"};
// end of tag type enumerations and names

char tagdata[MAX_TAGS][32];      // space for raw tag data unparsed
char tagname[MAX_TAGS][24];      // tag names
char tagmac[MAX_TAGS][18];       // tag macs
int  tagrssi[MAX_TAGS];          // RSSI for each tag

uint8_t tagtype[MAX_TAGS];       // "cached" value for tag type
uint8_t tagcount = 0;            // total amount of known tags
uint8_t scanning = 0;            // flag for scantime
int interval = 1;
time_t lastpublish = 0;

// Default hostname base. Last 3 octets of MAC are added as hex.
// The hostname can be changed explicitly from the portal.
char myhostname[64] = "esp32-ble2mqtt-";

TaskHandle_t mqtttask = NULL;

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
IPAddress apIP(192,168,4,1);                     // portal ip address
const char my_ssid[] PROGMEM = "ESP32 BLE2MQTT"; // AP SSID
uint32_t portal_timer = 0;

char heardtags[MAX_TAGS][18];
uint8_t heardtagtype[MAX_TAGS];

File file;
BLEScan* blescan;

/* ------------------------------------------------------------------------------- */
/* Get known tag index from MAC address. Format: 12:34:56:78:9a:bc */
uint8_t getTagIndex(const char *mac) {
    for (uint8_t i = 0; i < MAX_TAGS; i++) {
        if (strcmp(tagmac[i],mac) == 0) {
            return i;
        }
    }
    return 0xFF; // no tag with this mac found
}

/* ------------------------------------------------------------------------------- */
/* Detect tag type from payload and mac
 *  
 * Ruuvi tags (Manufacturer ID 0x0499) with data format V5 only
 *
 * Homemade tags (Manufacturer ID 0x02E5 Espressif Inc)
 *   The sketches are identified by next two bytes after MFID.
 *   0xE948 water tank gauge https://github.com/oh2mp/esp32_watersensor/
 *   0x1A13 thermocouple sensor for gas flame https://github.com/oh2mp/esp32_max6675_beacon/
 *   0xACDC energy meter pulse counter 
 *   
 * Xiaomi Mijia thermometer with atc1441 custom firmware. 
 *   https://github.com/atc1441/ATC_MiThermometer
 *
 */

uint8_t tagTypeFromPayload(const uint8_t *payload, const uint8_t *mac) {
    // Has manufacturerdata? If so, check if this is known type.
    if (memcmp(payload,"\x02\x01\x06",3) == 0 && payload[4] == 0xFF) {
        if (memcmp(payload+5,"\x99\x04\x05",3) == 0)      return TAG_RUUVI;
        if (memcmp(payload+5,"\xE5\x02\xDC\xAC",4) == 0)  return TAG_ENERGY;
        if (memcmp(payload+5,"\xE5\x02\x48\xE9",4) == 0)  return TAG_WATER;
        if (memcmp(payload+5,"\xE5\x02\x13\x1A",4) == 0)  return TAG_THCPL;
    }
    // ATC_MiThermometer? The data should contain 10161a18 in the beginning and mac at offset 4.
    if (memcmp(payload,"\x10\x16\x1A\x18",4) == 0 && memcmp(mac,payload+4,6) == 0) return TAG_MIJIA;

    return 0xFF; // unknown
}

/* ------------------------------------------------------------------------------- */
/* Known devices callback
/* ------------------------------------------------------------------------------- */

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advDev) {
        set_led(0,0,128);
        uint8_t payload[32];
        uint8_t taginx = getTagIndex(advDev.getAddress().toString().c_str());

        // we are interested about known and saved BLE devices only
        if (taginx == 0xFF || tagname[taginx][0] == 0) return;

        memset(payload,0,32);
        memcpy(payload, advDev.getPayload(), 32);
        memset(tagdata[taginx],0,sizeof(tagdata[taginx]));
        
        // Don't we know the type of this device yet?
        if (tagtype[taginx] == 0) {
            uint8_t mac[6];
            tagtype[taginx] = tagTypeFromPayload(payload,mac);
        }
        // Copy the payload to tagdata
        memcpy(tagdata[taginx],payload,32); 

        tagrssi[taginx] = advDev.getRSSI();

        Serial.printf("BLE callback: payload=");
        for (uint8_t i = 0; i < 32; i++) {
             Serial.printf("%02x",payload[i]);
        }
        Serial.printf("; ID=%d; type=%d; addr=%s; name=%s\n",taginx, tagtype[taginx], tagmac[taginx], tagname[taginx]);
        set_led(0,0,0);
    }
};

/* ------------------------------------------------------------------------------- */
/* Find new devices when portal is started
/* ------------------------------------------------------------------------------- */
class ScannedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advDev) {
        set_led(0,0,128);
        uint8_t payload[32];
        uint8_t taginx = getTagIndex(advDev.getAddress().toString().c_str());

        Serial.printf("Heard %s %s\nPayload: ",advDev.toString().c_str(),advDev.getName().c_str());
        
        memcpy(payload, advDev.getPayload(), 32);
        for (uint8_t i = 0; i < 32; i++) {
             Serial.printf("%02x",payload[i]);
        }    
        Serial.printf("\n");

        // skip known tags, we are trying to find new
        if (taginx != 0xFF) return;
               
        /* we are interested only about Ruuvi tags (Manufacturer ID 0x0499)
         *  and self made tags that have Espressif ID 0x02E5
         *  and Xiaomi Mijia thermometer with atc1441 custom firmware
         */
        uint8_t mac[6];
        memcpy(mac,advDev.getAddress().getNative(),6);
        uint8_t htype = tagTypeFromPayload(payload,mac);         
        
        if (htype != 0xFF && htype != 0) {
            for (uint8_t i = 0; i < MAX_TAGS; i++) {
                 if (strlen(heardtags[i]) == 0) {
                     strcpy(heardtags[i],advDev.getAddress().toString().c_str());
                     heardtagtype[i] = htype;
                     Serial.printf("Heard new tag: %s %s\n",heardtags[i],type_name[htype]);
                     break;
                 }
            }
        } else {
            Serial.print("Ignoring unsupported device.\n");
        }
        set_led(0,0,0);
    }
};

/* ------------------------------------------------------------------------------- */
void loadWifis() {
    if (SPIFFS.exists("/known_wifis.txt")) {
        char ssid[33];
        char pass[65];
        
        file = SPIFFS.open("/known_wifis.txt", "r");
        while (file.available()) {
            memset(ssid,'\0',sizeof(ssid));
            memset(pass,'\0',sizeof(pass));
            file.readBytesUntil('\t', ssid, 32);
            file.readBytesUntil('\n', pass, 64);
            WiFiMulti.addAP(ssid, pass);
            Serial.printf("wifi loaded: %s / %s\n",ssid,pass);
        }
        file.close();
    }
    if (SPIFFS.exists("/myhostname.txt")) {
        file = SPIFFS.open("/myhostname.txt", "r");
        memset(myhostname, 0, sizeof(myhostname));
        file.readBytesUntil('\n', myhostname, sizeof(myhostname));
        file.close();
    }
    Serial.printf("My hostname: %s\n",myhostname);
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

    if (SPIFFS.exists("/known_tags.txt")) {
        uint8_t foo = 0;
        file = SPIFFS.open("/known_tags.txt", "r");
        while (file.available()) {
            memset(sname, '\0', sizeof(sname));
            memset(smac, '\0', sizeof(smac));
            
            file.readBytesUntil('\t', smac, 18);
            file.readBytesUntil('\n', sname, 25);
            while (isspace(smac[strlen(smac)-1]) && strlen(smac) > 0) smac[strlen(smac)-1] = 0;
            while (isspace(sname[strlen(sname)-1]) && strlen(sname) > 0) sname[strlen(sname)-1] = 0;
            if (sname[strlen(sname)-1] == 13) sname[strlen(sname)-1] = 0;
            strcpy(tagmac[foo],smac);
            strcpy(tagname[foo],sname);
            foo++;
            if (foo >= MAX_TAGS) break;
            tagcount++;
        }
        file.close();
    }
}
/* ------------------------------------------------------------------------------- */
void loadMQTT() {
    if (SPIFFS.exists("/mqtt.txt")) {
        char tmpstr[8];
        memset(tmpstr, 0, sizeof(tmpstr));
        memset(mqtt_host, 0, sizeof(mqtt_host));
        memset(mqtt_user, 0, sizeof(mqtt_user));
        memset(mqtt_pass, 0, sizeof(mqtt_pass));
        memset(topicbase, 0, sizeof(topicbase));
        
        file = SPIFFS.open("/mqtt.txt", "r");
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
        Serial.printf("MQTT broker: %s:%d - topic prefix: %s\n",mqtt_host,mqtt_port,topicbase);
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
        if (r > 0 && b == 0) {r--; g++;}
        if (g > 0 && r == 0) {g--; b++;}
        if (b > 0 && g == 0) {r++; b--;}
        r = constrain(r,0,128); g = constrain(g,0,128); b = constrain(b,0,128);
        set_led(r,g,b);
        delay(1);
    }
}
/* ------------------------------------------------------------------------------- */
void setup() {
    Serial.begin(115200);
    Serial.println("\n\nESP32 BLE2MQTT");

    pinMode(BUTTON, INPUT_PULLUP);

    // Append last 3 octets of MAC to the default hostname
    uint8_t mymac[6];
    esp_read_mac(mymac, (esp_mac_type_t)0); // 0:wifi station, 1:wifi softap, 2:bluetooth, 3:ethernet
    char mac_end[8];
    sprintf(mac_end,"%02x%02x%02x",mymac[3],mymac[4],mymac[5]);
    strcat(myhostname,mac_end);

    ledcSetup(LED_R, 5000, 8);
    ledcAttachPin(LED_R_PIN, LED_R);
    ledcSetup(LED_G, 5000, 8);
    ledcAttachPin(LED_G_PIN, LED_G);
    ledcSetup(LED_B, 5000, 8);
    ledcAttachPin(LED_B_PIN, LED_B);
    led_fx();
    set_led(0,0,0);
    
    for (uint8_t i = 0; i < MAX_TAGS; i++) {
        memset(tagname[i],0,sizeof(tagname[i]));
        memset(tagdata[i],0,sizeof(tagdata[i]));
        memset(tagmac[i],0,sizeof(tagmac[i]));
        tagrssi[i] = 0;
        tagtype[i] = 0;
    }
    
    SPIFFS.begin();
    loadSavedTags();
    loadMQTT();
    
    BLEDevice::init("");
    blescan = BLEDevice::getScan();

    if (tagcount == 0) startPortal();
    
    blescan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    blescan->setActiveScan(true);
    blescan->setInterval(100);
    blescan->setWindow(99);

    loadWifis();
    client.setServer(mqtt_host, mqtt_port);

    // https://github.com/espressif/arduino-esp32/issues/2537#issuecomment-508558849
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    WiFi.setHostname(myhostname);
    
    // Reset real time clock
    timeval epoch = {0, 0};
    const timeval *tv = &epoch;
    settimeofday(tv, NULL);
}

/* ------------------------------------------------------------------------------- */
void loop() {
    uint8_t do_send = 0;
    
    if (portal_timer == 0) {
        if (time(NULL) == 0 || interval == 0) do_send = 1;
        if (interval > 0) {
            if ((time(NULL) - lastpublish) >= (interval*60)) do_send = 1;
        }
        if (digitalRead(BUTTON) == LOW) {
            do_send = 0;
            startPortal();
        }
        if (time(NULL) % 60 == 0) {
            /* Current official Ruuvi firmware (v 2.5.9) https://lab.ruuvi.com/dfu/ broadcasts 
             * in every 6425ms in RAWv2 mode, so 11 seconds should be enough to hear all tags 
             * unless you have really many.
             */

            set_led(0,128,128);
            Serial.printf("Start scan at unixtime: %d\n",time(NULL));
            BLEScanResults foundDevices = blescan->start(11, false);
            blescan->clearResults();
            set_led(128,0,128);
            delay(250);
            set_led(0,0,0);
            
            /* Something is wrong if zero known tags is heard, so then reboot. 
             * Possible if all of them are out of range too, but that should not happen anyway.
             */
             if (foundDevices.getCount() == 0 && tagcount > 0) {
                 set_led(128,0,0);
                 ESP.restart();
             }
        }
    }
    if (do_send == 1) mqtt_send();
    
    if (portal_timer > 0) {     // are we in portal mode?
        if (millis() % 500 < 250) {
            set_led(int((millis()-portal_timer)/(APTIMEOUT/128)),128-int((millis()-portal_timer)/(APTIMEOUT/64)*2),0);
        } else {
            set_led(0,0,0);
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
                    temperature = ((short)tagdata[curr_tag][8]<<8) | (unsigned short)tagdata[curr_tag][9];
                    humidity = ((unsigned short)tagdata[curr_tag][10]<<8) | (unsigned short)tagdata[curr_tag][11];
                    foo = ((unsigned short)tagdata[curr_tag][20] << 8) + (unsigned short)tagdata[curr_tag][21];
                    voltage = ((double)foo / 32  + 1600);
                    pressure = ((unsigned short)tagdata[curr_tag][12]<<8) + (unsigned short)tagdata[curr_tag][13] + 50000;

                    sprintf(json,"{\"type\":%d,\"t\":%d,\"rh\":%d,\"bu\":%d,\"ap\":%d,\"s\":%d}",
                            tagtype[curr_tag],int(temperature*.05),int((float)humidity*.0025),
                            voltage, int(pressure/100), abs(tagrssi[curr_tag]));
                }
            }
            // Other tags --------------------------------------------------------------------------------------
            // water gauge
            if (tagtype[curr_tag] == TAG_WATER) {
                if (tagdata[curr_tag][0] != 0) {                    
                    sprintf(json,"{\"type\":%d,\"lv\":%d,\"s\":%d}",
                            tagtype[curr_tag],(unsigned int)tagdata[curr_tag][10],abs(tagrssi[curr_tag]));
                }
            }
            // flame thermocouple
            if (tagtype[curr_tag] == TAG_THCPL) {
                // get the temperature
                foo = (((unsigned short)tagdata[curr_tag][10] << 8) + (unsigned short)tagdata[curr_tag][9]) >> 2;
                sprintf(json,"{\"type\":%d,\"t\":%d,\"s\":%d}",tagtype[curr_tag],foo*10,abs(tagrssi[curr_tag]));
            }
            // energy meter pulse counter
            if (tagtype[curr_tag] == TAG_ENERGY) {
                if (tagdata[curr_tag][0] != 0) {
                    wh = (((uint32_t)tagdata[curr_tag][16] << 24) + ((uint32_t)tagdata[curr_tag][15] << 16) 
                          + ((uint32_t)tagdata[curr_tag][14] << 8) + (uint32_t)tagdata[curr_tag][13]);

                    wht = (((uint32_t)tagdata[curr_tag][12] << 24) + ((uint32_t)tagdata[curr_tag][11] << 16) 
                           + ((uint32_t)tagdata[curr_tag][10] << 8) + (uint32_t)tagdata[curr_tag][9]);
                      
                    sprintf(json,"{\"type\":%d,\"e\":%d,\"et\":%d,\"s\":%d}",tagtype[curr_tag],wh,wht,abs(tagrssi[curr_tag]));
                }
            }
            // Xiaomi Mijia thermometer with ATC_MiThermometer custom firmware by atc1441
            if (tagtype[curr_tag] == TAG_MIJIA) {
                if (tagdata[curr_tag][0] != 0) {
                    temperature = ((short)tagdata[curr_tag][10]<<8) | (unsigned short)tagdata[curr_tag][11];
                    // sprintf(json,"%.1f\x29",temperature*0.1); // 0x29 = degree sign in the bigfont
                    humidity = (unsigned short)tagdata[curr_tag][12];
                    voltage = ((short)tagdata[curr_tag][14]<<8) | (unsigned short)tagdata[curr_tag][15];
                    sprintf(json,"{\"type\":%d,\"t\":%d,\"rh\":%d,\"bu\":%d,\"s\":%d}",
                            tagtype[curr_tag],int(temperature),int(humidity),int(voltage),abs(tagrssi[curr_tag]));
                }              
            }
            if (json[0] != 0) {
                memset(topic,0,sizeof(topic));
                sprintf(topic, "%s/%s",topicbase,tagname[curr_tag]);
            
                // convert possible spaces to underscores in topic
                for (uint8_t i = 0; i < strlen(topic); i++) {
                    if (topic[i] == 32) topic[i] = '_';
                }
                
                if (WiFiMulti.run() == WL_CONNECTED) {
                    if (curr_tag == 0) {
                        Serial.printf("Connected to SSID=%s - My IP=%s\n",
                                  WiFi.SSID().c_str(),WiFi.localIP().toString().c_str());
                        Serial.flush();
                    }
                    if (client.connect(myhostname,mqtt_user,mqtt_pass)) {
                        if (client.publish(topic, json)) {
                            set_led(0,128,0);
                            Serial.printf("%s %s\n",topic,json);
                            Serial.flush();
                            memset(tagdata[curr_tag],0,sizeof(tagdata[curr_tag]));
                            lastpublish = time(NULL);
                            published = true;                 
                        } else { 
                            set_led(51,0,0);
                            Serial.print("Failed to publish MQTT, rc=");
                            Serial.println(client.state());
                        }
                    } else {
                        set_led(51,0,0);
                        Serial.printf("Failed to connect MQTT broker, state=%d",client.state());
                    }
                } else {
                    set_led(128,0,0);
                    Serial.printf("Failed to connect WiFi, status=%d\n", WiFi.status());
                }
            }
        }
        memset(json,0,sizeof(json));
        delay(100);
    }
    // If we published something, disconnect the client here to clean session.
    if (published) client.disconnect();
    set_led(0,0,0);
}
/* ------------------------------------------------------------------------------- */
/* Portal code begins here
 *  
 *   Yeah, I know that String objects are pure evil 😈, but this is meant to be
 *   rebooted immediately after saving all parameters, so it is quite likely that 
 *   the heap will not fragmentate yet. 
 */
/* ------------------------------------------------------------------------------- */

void startPortal() {
   
    Serial.print("Starting portal...");
    portal_timer = millis();
    
    for (uint8_t i = 0; i < MAX_TAGS; i++) {
        memset(heardtags[i],0,sizeof(heardtags[i]));
    }
    Serial.print("\nListening 11 seconds for new tags...\n");

    // First listen 11 seconds to find new tags.
    set_led(0,128,128);
    blescan->setAdvertisedDeviceCallbacks(new ScannedDeviceCallbacks());
    blescan->setActiveScan(true);
    blescan->setInterval(100);
    blescan->setWindow(99);
    BLEScanResults foundDevices = blescan->start(11, false);
    blescan->stop();
    blescan->clearResults();
    blescan = NULL;
    BLEDevice::deinit(true);
    set_led(0,0,0);
        
    portal_timer = millis();

    WiFi.disconnect();
    delay(100);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(my_ssid);
    delay(2000);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        
    server.on("/", httpRoot);
    server.on("/style.css", httpStyle);
    server.on("/sensors.html", httpSensors);
    server.on("/savesens",httpSaveSensors);
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
    String html;

    file = SPIFFS.open("/index.html", "r");
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
    char pass[33];
    int counter = 0;
    
    portal_timer = millis();
    memset(tablerows, '\0', sizeof(tablerows));
    
    file = SPIFFS.open("/wifis.html", "r");
    html = file.readString();
    file.close();
    
    if (SPIFFS.exists("/known_wifis.txt")) {
        file = SPIFFS.open("/known_wifis.txt", "r");
        while (file.available()) {
            memset(rowbuf, '\0', sizeof(rowbuf)); 
            memset(ssid, '\0', sizeof(ssid));
            memset(pass, '\0', sizeof(pass));
            file.readBytesUntil('\t', ssid, 33);
            file.readBytesUntil('\n', pass, 33);
            sprintf(rowbuf,"<tr><td>SSID</td><td><input type=\"text\" name=\"ssid%d\" maxlength=\"32\" value=\"%s\"></td></tr>",counter,ssid);
            strcat(tablerows,rowbuf);
            sprintf(rowbuf,"<tr><td>PASS</td><td><input type=\"text\" name=\"pass%d\" maxlength=\"32\" value=\"%s\"></td></tr>",counter,pass);
            strcat(tablerows,rowbuf);
            counter++;
        }
        file.close();
    }
    if (SPIFFS.exists("/myhostname.txt")) {
        file = SPIFFS.open("/myhostname.txt", "r");
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
    String html;
        
    file = SPIFFS.open("/known_wifis.txt", "w");
    
    for (int i = 0; i < server.arg("counter").toInt(); i++) {
         if (server.arg("ssid"+String(i)).length() > 0) {
             file.print(server.arg("ssid"+String(i)));
             file.print("\t");
             file.print(server.arg("pass"+String(i)));
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
        file = SPIFFS.open("/myhostname.txt", "w");
        file.print(server.arg("myhostname"));
        file.print("\n");
        file.close();
    }
    
    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "2;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpMQTT() {
    portal_timer = millis();
    String html;
    
    file = SPIFFS.open("/mqtt.html", "r");
    html = file.readString();
    file.close();
    
    html.replace("###HOSTPORT###", String(mqtt_host)+":"+String(mqtt_port));
    html.replace("###USERPASS###", String(mqtt_user)+":"+String(mqtt_pass));
    html.replace("###TOPICBASE###", String(topicbase));
    html.replace("###INTERVAL###", String(interval));
       
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */
void httpSaveMQTT() {
    portal_timer = millis();
    String html;

    file = SPIFFS.open("/mqtt.txt", "w");
    file.printf("%s\n",server.arg("hostport").c_str());
    file.printf("%s\n",server.arg("userpass").c_str());
    file.printf("%s\n",server.arg("topicbase").c_str());
    file.printf("%s\n",server.arg("interval").c_str());
    file.close();
    loadMQTT(); // reread

    file = SPIFFS.open("/ok.html", "r");
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

    file = SPIFFS.open("/sensors.html", "r");
    html = file.readString();
    file.close();

    loadSavedTags();

    for(int i = 0 ; i < MAX_TAGS; i++) {
        if (strlen(tagmac[i]) == 0) continue;

        sprintf(rowbuf,"<tr><td colspan=\"2\" id=\"mac%d\">%s</td></tr><tr>\n",
                       counter,tagmac[i]);
        tablerows += String(rowbuf);
        sprintf(rowbuf,"<tr><td><input type=\"text\" id=\"sname%d\" name=\"sname%d\" maxlength=\"24\" value=\"%s\">",
                       counter,counter,tagname[i]);
        tablerows += String(rowbuf);
        sprintf(rowbuf,"<input type=\"hidden\" id=\"saddr%d\" name=\"saddr%d\" value=\"%s\">",counter,counter,tagmac[i]);
        tablerows += String(rowbuf);
        if (counter > 0) {
            sprintf(rowbuf,"<td><a onclick=\"moveup(%d)\">\u2191</a></td></tr>\n",counter);
            tablerows += String(rowbuf);
        } else {
            tablerows += "<td></td></tr>\n";
        }
        counter++;
    }
    if (strlen(heardtags[0]) != 0 && counter < MAX_TAGS) {
        for(int i = 0; i < MAX_TAGS; i++) {
            if (strlen(heardtags[i]) == 0) continue;
            if (getTagIndex(heardtags[i]) != 0xFF) continue;

            sprintf(rowbuf,"<tr><td colspan=\"2\" id=\"mac%d\">%s &nbsp; %s</td></tr>\n",
                    counter,heardtags[i],type_name[heardtagtype[i]]);
            tablerows += String(rowbuf);
            sprintf(rowbuf,"<tr><td><input type=\"text\" id=\"sname%d\" name=\"sname%d\" maxlength=\"24\">",counter,counter);
            tablerows += String(rowbuf);
            sprintf(rowbuf,"<input type=\"hidden\" id=\"saddr%d\" name=\"saddr%d\" value=\"%s\">",
                           counter,counter,heardtags[i]);
            tablerows += String(rowbuf);
            if (counter > 0) {
                sprintf(rowbuf,"<td><a onclick=\"moveup(%d)\">\u2191</a></td></tr>\n",counter);
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
    String html;
        
    file = SPIFFS.open("/known_tags.txt", "w");
    
    for (int i = 0; i < server.arg("counter").toInt(); i++) {
         if (server.arg("sname"+String(i)).length() > 0) {
             file.print(server.arg("saddr"+String(i)));
             file.print("\t");
             file.print(server.arg("sname"+String(i)));
             file.print("\n");
         }
    }
    file.close();
    loadSavedTags(); // reread

    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "2;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpStyle() {
    portal_timer = millis();
    String css;

    file = SPIFFS.open("/style.css", "r");
    css = file.readString();
    file.close();       
    server.send(200, "text/css", css);
}

/* ------------------------------------------------------------------------------- */
void httpBoot() {
    portal_timer = millis();
    String html;
    
    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();
    
    server.sendHeader("Refresh", "2;url=about:blank");
    server.send(200, "text/html; charset=UTF-8", html);
    delay(1000);
    
    ESP.restart();
}
/* ------------------------------------------------------------------------------- */
