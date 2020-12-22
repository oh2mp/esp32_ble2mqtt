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
