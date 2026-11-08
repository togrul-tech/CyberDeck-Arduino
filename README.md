# CyberDeck-Arduino
A cyberdeck project, contains arduino esp32, tft display, some cables, and a bluetooth keyboard


CyberDeck OS v3.0 — User Guide (ESP32 DevKitC)

TFT Display Wiring

The TFT display has 8 pins: LED, SCK, SDA, A0, RESET, CS, GND, VCC.

Display Pin
	

ESP32 Pin

VCC
	

3V3

GND
	

GND

CS
	

GPIO 5

RESET
	

GPIO 4

A0
	

GPIO 2

SDA
	

GPIO 23

SCK
	

GPIO 18

LED
	

3V3

No other components are connected. The keyboard is used for control.

Arduino IDE Setup

Select Tools → Board → ESP32 Dev Module.

Select Tools → Partition Scheme → Huge APP (3MB No OTA). Required: the firmware is 1.38 MB and does not fit in the default partition scheme (“Sketch too big” error).

Install these libraries through Library Manager:

Adafruit GFX Library

Adafruit ST7735 and ST7789 Library

Adafruit ILI9341

NimBLE-Arduino (2.x)

Upload the firmware. When “Connecting…” appears, press and hold the BOOT button on the board.

Keyboard: Logitech Pebble Keys 2 K380s

First-time pairing: Press and hold F1, F2, or F3 for 3 seconds. If prompted for a pairing code, type 123456 and press Enter.

Subsequent connections: Press any key; the keyboard reconnects automatically.

Indicators in the top-right corner: - K — Keyboard connected - W — Wi-Fi connected

Controls

Action
	

Key

Navigate
	

↑ / ↓

Select
	

Enter

Back
	

Esc

Previous commands in Terminal
	

↑ / ↓

Scroll a web page by line
	

↑ / ↓

Scroll a web page by page
	

← / → or Space

Menu

WIFI SCANNER

CHANNEL MAP

BLE SCANNER

KEYBOARD

TERMINAL

SYSTEM INFO

CYBER WORM

TYPE HACK

MATRIX

Connecting to Wi-Fi

Method 1: Wi-Fi Scanner

Open WIFI SCANNER.

Select a network using ↑ / ↓.

Press Enter.

Enter the password (it appears as **** on screen).

Press Enter.

Method 2: Terminal

wifi            # Show a numbered list of networks
connect 2       # Connect to network number 2; then enter the password and press Enter

The password is saved, and the ESP32 reconnects automatically next time. To erase the saved network, use forget.

Terminal Commands

Command
	

Description

help
	

Show all commands

wifi / connect <n>
	

List networks / connect to a network

status / ip / myip
	

Connection status / local IP / public IP

disconnect / forget
	

Disconnect Wi-Fi / erase saved network

google <word>
	

Search Google (text-only)

ddg <word>
	

Search DuckDuckGo (if Google blocks the request)

get <site>
	

Open a website, e.g. get example.com

weather <city>
	

Get weather, e.g. weather Baku

joke
	

Random joke (in English)

time
	

Get the accurate time from the internet (Baku time)

dns <site>
	

Resolve a website to its IP address

ble
	

Scan for nearby Bluetooth devices

info / uptime / sys
	

System information

calc <expression>
	

Calculator, e.g. calc (2+3)*4^2

roll / coin
	

Roll a die / flip a coin

guess
	

Guess a number from 1 to 100

snake / hack / matrix
	

Games and visual effects

history / clear / echo
	

Command history / clear screen / print text

menu / reboot
	

Return to menu / restart

Games

CYBER WORM

Use the arrow keys to control the worm. Eat the purple dots, and avoid hitting the walls or yourself. The worm speeds up as it eats.

TYPE HACK

Words labeled “hacker” fall from the top of the screen. Type a word to destroy it. If 3 words reach the bottom, the game ends. Press Enter to clear the text you have typed.

Guess (Terminal)

Choose a number and use the TOO BIG / TOO SMALL hints to find it.

Best scores are saved in memory.
