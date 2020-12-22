# ESP32 BLE2MQTT configuration file formats

Here is the specification of the configuration files that the ESP32 BLE2MQTT uses.
All files use only newline aka Unix line break. Windows line break CRLF will cause problems.

## mqtt.txt

row 1: ip:port or host:port. Must be separated by colon.

row 2: username:password for the MQTT broker. They must be separated by colon.

row 3: topic prefix. This is the base for the topic and sensor name is added automatically after this.

row 4: MQTT publish interval in minutes.

All rows must end in newline.

**Example mqtt.txt file:**

```
192.168.36.99:1883
publisher:password123
home/sensors
5
```

## known_tags.txt

One known tag per row. First the MAC address in lowercase hex and colons between bytes, then TAB, 
then name of the tag and newline.

**Example known_tags.txt file:**

```
f4:01:83:12:ce:95	foo
e3:28:8c:99:47:ae	bar
```

## known_wifis.txt

One known WiFi network per row. First the SSID, then TAB, then password and newline.

**Example known_wifis.txt**

```
OH2MP	MyVerySecretPass123
OH2MP-5	AnotherVerySecretPass456
```
