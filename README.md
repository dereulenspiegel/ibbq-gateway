# iBBQ-Gateway

This small projects is tool to extend the range and functionality of simple Bluetooth based BBQ thermometers. It is based
around the ESP32 and uses the Bluetooth connectivity of the ESP32 to connect to iBBQ like BBQ thermometers and read their
values. At the same time it either connects to a configured WiFi or creates an access point you can connect to to access
a web UI displaying the values of the connected thermometer.

## Acknowledgments

This project is heavily inspired by the [WLANThermo](https://wlanthermo.de/) project. The web UI of the
[WLANThermo nano](https://github.com/WLANThermo-nano/WLANThermo_nano_Software) is even shamelessly copied
(in line with the license :) ) and modified for this project.

## Requirements

* An ESP32-Board with at least 4MBytes Flash
* A computer with Linux, macOS or Windows
* The ESP-IDF (https://docs.espressif.com/projects/esp-idf/en/stable/get-started/index.html)
* [mkspiffs](https://github.com/igrr/mkspiffs)

## Getting started

* Clone this repository somewhere
* Connect the ESP32 board to your computer
* [Setup](https://docs.espressif.com/projects/esp-idf/en/stable/get-started/index.html) the ESP-IDF 
* Set the correct USB port via `make menuconfig` (see ESP-IDF documentation)
* Compile or install [mkspiffs](https://github.com/igrr/mkspiffs)
* Change into the directory where you cloned this repository
* Execute `make flash_spiffs`, wait for it to finish
* Execute `make flash`, wait for it to finish

## Usage

After flashing the device you should find a new WiFi with the name `ibbq-ap`. Connecting to this WiFi with the
password `ibbq-wifi`. In your browser enter `http://ibbq.gateway` and access the web UI. You can now click in the menu
on WiFi and should see nearby WiFi networks. This allows you to select one, enter the password and save it as the network
to connect to. After the ESP32 has restart you can access the web UI again (check you WiFi AP for the IP) and if an iBBQ
thermometer is nearby it should connect automatically

## Features

* Automatically connects to iBBQ Bluetooth BBQ thermometers (tested with IBT-2X)
* Adapts amount of displayed channels on web UI to amount of actual channels of connected thermometer
* Announces `ibbq-server` mDNS HTTP service
* Should work with most ESP32 boards available
* Should work with iBBQ based Bluetooth BBQ thermometers with up to 8 channels
* Channel configuration. Give each channel a name, color and min/max temperatur
* Set custom hostname
* Set custom access point name
* If configured WiFi is not reachable, fallback to access point mode after 5 retries. This disables BLE functionality

## Limitations

* BLE and soft AP mode don't work together. It does not seem to be possible to use this without an existing WiFi
  network
* Currently only one thermometer can be connected

## ToDos

* [ ] Generate WiFi password and use OLED to display it there (increases security)
* [ ] Implement notifications with the help of Progressive Web App technology
* [ ] Improve BLE connection reliability
* [ ] Implement 'local' notifications
* [ ] Add MQTT connectivity
* [ ] Add Pitmaster functionality
