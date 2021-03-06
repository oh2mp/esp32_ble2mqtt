# Example configuration for Mosquitto and Telegraf

### [Mosquitto](https://mosquitto.org/)

With this configuration Mosquitto listens its default port tcp/1883

Full documentation for `mosquitto.conf` is [available at mosquitto.org](https://mosquitto.org/man/mosquitto-conf-5.html)

Remember to setup pwfile for passwords too.


```
allow_duplicate_messages true
password_file /etc/mosquitto/pwfile
socket_domain ipv4
```

### [Telegraf](https://docs.influxdata.com/telegraf/v1.17/)

#### InfluxDB output section

It's assumed here that you have set up database `home` and user `telegraf` with appropriate permissions
to your InfluxDB.


```
[[outputs.influxdb]]
  urls = ["https://127.0.0.1:8086"]

  database = "home"
  exclude_database_tag = false
  skip_database_creation = true

  username = "telegraf"
  password = "telegraf_influxdb_user_password_here"

  insecure_skip_verify = true

```

#### MQTT input section

It's assumed here that you have added user `telegraf` to Mosquitto's pwfile. 

This example configuration subscribes to all topics and sets `type` parameter in data as a tag. 
See parameters etc. from [DATAFORMATS.md](DATAFORMATS.md).

The MQTT topic is inserted as tag `sensor` to InfluxDB

The full documentation of the MQTT Consumer input plugin is 
[available at influxdata's github](https://github.com/influxdata/telegraf/blob/release-1.17/plugins/inputs/mqtt_consumer/README.md)


```
[[inputs.mqtt_consumer]]
    servers = ["tcp://127.0.0.1:1883"]
    topics = ["#"]

    tag_keys = ["type"]

    topic_tag = "sensor"
    qos = 0

    username = "telegraf"
    password = "telegraf_password_from_mosquitto_pwfile"

    data_format = "json"
    name_override = "sensors"
    json_strict = false 

```