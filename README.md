# OH2MP ESP32 BLE2MQTT

### An ESP32 based gateway that listens BLE beacons and sends the data via MQTT

Web-configurable BLE data collector that sends data to a MQTT broker. In my own configuration I have
Mosquitto as a broker and InfluxDB + Telegraf with MQTT plugin. See [EXAMPLES.md](EXAMPLES.md). 

This software sends data as JSON to the broker. The data is specified to be compact to avoid eg. high bills
when this is used with a mobile internet with some data plan. See [DATAFORMATS.md](DATAFORMATS.md)

BLE beacons that are currently supported:

- [Ruuvi tag](https://ruuvi.com/) (Data format V5 aka RAWv2 only)
- [Xiaomi Mijia Bluetooth Thermometer 2 with ATC_MiThermometr firmware](https://github.com/atc1441/ATC_MiThermometer) (stock firmware not supported)
- [ESP32 Water sensor](https://github.com/oh2mp/esp32_watersensor)
- [ESP32 Energy meter](https://github.com/oh2mp/esp32_energymeter)
- [ESP32 MAX6675 beacon for thermocouples](https://github.com/oh2mp/esp32_max6675_beacon)

This is partly based on the same code as [OH2MP ESP32 Smart RV](https://github.com/oh2mp/esp32_smart_rv)
and [OH2MP ESP32 Ruuvicollector](https://github.com/oh2mp/esp32_ruuvicollector)

------

## Software prerequisities

- Some MQTT broker like Mosquitto running somewhere.
- [Arduino IDE](https://www.arduino.cc/en/main/software)
- [Arduino ESP32 filesystem uploader](https://github.com/me-no-dev/arduino-esp32fs-plugin/)

Choose correct ESP32 board and change partitioning setting:<br /> **Tools -> Partition Scheme -> Huge APP(3MB No OTA)**

Use the filesystem uploader tool to upload the contents of data library. It contains the html pages for
the configuring portal.

By default the software assumes that there are maximum 16 beacons or tags, but this can be changed from the code,
see row `#define MAX_TAGS 16`


## Configuration option

The portal saves all configurations onto the SPIFFS filesystem. They are just text files, so you can
precreate them and then your ESP32 Ruuvi Collector is preconfigured and you dont' have to use the portal
at all. Just place yout configuration files into the data directory along the html files and 
upload them with ESP filesystem uploader.

See [FORMATS.md](FORMATS.md).

## LED behavior

Optionally an RGB LED can be connected to the board. It acts as a status indicator. At boot the LED
shows a short color effect to see that it's working. Colors and meanings in operating mode:

- cyan = BLE scanning active, but no beacons heard yet
- blue = BLE beacon(s) heard
- purple = end of BLE scanning
- green = sending data to MQTT broker
- red = cannot connect to WiFi
- orange = WiFi connection works but cannot send data to MQTT broker

The LED pins are configurable from `#define` rows. The defaults are 21 red, 22 green and 23 blue.
Every one should be connected with a eg. 1kΩ resistor.

------

## Portal mode

If the GPIO0 is grounded (same as BOOT button is pressed), the ESP32 starts portal mode.
The pin can be also changed from the code, see row `#define APREQUEST 0`

In the start of portal mode the ESP32 is scanning 11 seconds for beacons. During the scan the color
behavior of the LED is similar like in operating mode.

WiFi AP is not listening yet at the scanning period. After the LED starts illuminating green, 
connect to WiFi **ESP32 BLE2MQTT**, accept that there's no internet connection
and take your browser to !http://192.168.4.1/

The web GUI should be self explanatory. 

It's a good idea to find out the Bluetooth MAC addresses of the beacons beforehand. For Ruuvi tags the
easiest way is to use Ruuvi software. For other beacons eg. 
[BLE Scanner by Bluepixel Technologies](https://play.google.com/store/apps/details?id=com.macdom.ble.blescanner)
is a suitable app for Android.

The portal mode has a timeout. The unit will reboot after 2 minutes of inactivity and the remaining time
is visible on the screen. This timeout can be changed from line #define APTIMEOUT
The LED changes its color slowly from green to yellow and then red depending how near the timeout is.

There's almost no sanity checks for the data sent from the forms. This is not a public web service and if 
you want to mess up your board or try to make a denial of service using eg. buffer overflows, feel free to 
do so.

### Sample screenshots from the portal

![Portal main](s/portal.jpg)
![Sensors config](s/sensors_config.jpg)
![MQTT config](s/mqtt_config.jpg)

------

