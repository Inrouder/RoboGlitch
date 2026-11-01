# RoboGlitch 🤖⚡

ESP8266EX (4 MB) Robo-Wetterstation: animierte Augen, Uhr, WLAN-Status, Wetter über Open-Meteo, Einrichtungswebseite und OTA-Firmwareupdates. Die ST7789-Pinbelegung ist ein **vorläufiges Hardwareprofil**; bei abweichendem Display funktioniert die Firmware ggf. zunächst nur über Web/Serial.

## Flashen im Browser
Nach erfolgreicher GitHub-Actions-Build und einmaliger Freischaltung von **Settings → Pages → Source: GitHub Actions**:
**https://inrouder.github.io/RoboGlitch/**

Mit Chrome oder Edge am Desktop, über ein USB-Datenkabel. Anschließend ESP-Gerät auswählen. Alternative: die Actions-Artifacts enthalten eine fertig gebaute `firmware.bin`, die per esptool mit Offset `0x000000` geflasht werden kann.

## Erster Start
Wenn keine WLAN-Daten gespeichert sind, erscheint das WLAN `RoboGlitch-Setup`. Verbinden und `http://192.168.4.1/` im Browser öffnen. Dort WLAN, Ortsname sowie Breiten-/Längengrad festlegen. Im heimischen WLAN ist das Gerät über seine IP zu erreichen; `roboglitch.local` benötigt mDNS-Unterstützung.

OTA: `http://<geraete-ip>/update`, Zugangsdaten im Einrichtungsformular vergeben. **Bitte unbedingt ein OTA-Passwort vergeben**, bevor das Gerät in einem geteilten Netzwerk eingesetzt wird.

## Entwickeln
VS Code + PlatformIO: `pio run -e roboglitch`; USB-Upload: `pio run -e roboglitch -t upload --upload-port COM8`.
Die Firmware legt ihre Konfiguration in LittleFS ab. Nutzer sollten bei Versionswechseln das Flash-Layout beachten.

## Hardware
ESP8266EX, 26-MHz-Quarz, 4 MB Flash, vermutlich 240x240 ST7789. Vorläufige Displaypins: SCLK GPIO14, MOSI GPIO13, CS GPIO15, DC GPIO0, RST GPIO2, BL GPIO5. Das wird beim ersten Hardwaretest geprüft. GPIO0/GPIO2/GPIO15 beeinflussen den Bootmodus – keine externen Lasten unüberlegt hinzufügen.

Geplant: getrennte spätere Bewegungselektronik mit Gyro, Motorcontroller, Sensoren. Die bisherige PCB stellt nicht genügend GPIOs und keine robuste Motorstromversorgung bereit.
