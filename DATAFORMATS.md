# Dataformats used with OH2MP ESP32 BLE2MQTT

#### All of these should stay as they are specified, but they _may_ change.

------------

### Beacon type

In every MQTT packet an information about the beacon type is sent. It is just numerical and the types
supported at the moment are:

| Name       | Number | Description |
| ---------- |:------:| ----------- |
| TAG_RUUVI  | 1 | Ruuvi tag |
| TAG_MIJIA  | 2 | Xiaomi Mijia Thermometer 2 |
| TAG_ENERGY | 3 | OH2MP energy meter beacon |
| TAG_WATER  | 4 | OH2MP water gauge beacon |
| TAG_THCPL  | 5 | OH2MP thermocouple beacon |

These same numbers are used internally in [OH2MP Smart RV](https://github.com/oh2mp/esp32_smart_rv)

------------

### Fields in JSON messages

These field names are chosen so that they are short to make messages more compact. Some fields are
already specified for future use.

| Fieldname  | Unit    | Description |
| ---------- | ------- | ----------- |
| t          | 1/10 °C | temperature in "centiCelsius" |
| rh         | %       | relative humidity |
| ap         | hPa     | athmospheric pressure |
| bu         | mV      | battery voltage |
| e          | Wh      | energy consumption since last reset |
| et         | Wh      | energy consumption total |
| lv         | liter   | liquid volume |
| u          | mV      | electric voltage |
| i          | mA      | electric current |
| p          | mW      | electric power |
| m          | g       | mass (or weight in spoken language) |

--------------

