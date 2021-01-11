# System Nexa support for ESP32

This repository is (or someday will be) an esp-idf component that can be used in ESP32 projects. It aims for supporting the 433 Mhz band System Nexa wireless home automation products.

## Hardware

The program is developed on an Olimex ESP32-POE board, but will work on any ESP32 board with proper modifications. 

The 433Mhz radio transciever used is an inexpensive pair of transmitter and receiver, that seems to be commonly available on chinese webshops. The modulation techique used is "ON/OFF keying".

## Usage 

Connect the radio hardware as follows:

* GPIO5 (UEXT pin 10): RF transmitter data pin
* GPIO14 (UEXT pin 9): RF receiver data pin
