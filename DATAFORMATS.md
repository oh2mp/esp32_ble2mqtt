# Dataformats used with OH2MP ESP32 BLE2MQTT

### Beacon type

In every MQTT packet an information about the beacon type is sent. It is just numerical and the types
supported at the moment are:


| Name       | Number | Description |
| ---------- | ------ | ----------- |
| TAG_RUUVI  | 1 | Ruuvi tag |
| TAG_MIJIA  | 2 | Xiaomi Mijia Thermometer 2 |
| TAG_ENERGY | 3 | OH2MP energy meter beacon |
| TAG_WATER  | 4 | OH2MP water gauge beacon |
| TAG_THCPL  | 5 | OH2MP thermocouple beacon |
