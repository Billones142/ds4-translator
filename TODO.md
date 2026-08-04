- implement system to detect specific programs (native or wine) and apply a controller config based on that
- the program should be able to "inject" the controller to programs
- fix problem where disconecting and reconecting the controller makes it so the emulation stops (does not always happen), make it so the virtual controller exist until more that 300 seconds its disconected and it should remember the LED color when reconecting.
```
stefano@gamer-linux:~$ sudo systemctl status ds4-translator.service
[sudo] password for stefano:
● ds4-translator.service - DualShock 4 to Virtual Controller Translator Daemon
     Loaded: loaded (/etc/systemd/system/ds4-translator.service; enabled; preset: disabled)
     Active: active (running) since Mon 2026-08-03 22:02:26 -03; 55min ago
 Invocation: 090d6687ed2740578f8177ac5377cb63
   Main PID: 45396 (ds4-translator)
      Tasks: 1 (limit: 38144)
     Memory: 448K (peak: 3.2M)
        CPU: 20.329s
     CGroup: /system.slice/ds4-translator.service
             └─45396 /usr/local/bin/ds4-translator

ago 03 22:41:29 gamer-linux ds4-translator[45396]: UHID Device Opened by host
ago 03 22:55:26 gamer-linux ds4-translator[45396]: Physical controller connection dropped.
ago 03 22:55:31 gamer-linux ds4-translator[45396]: Found physical DualShock 4: /dev/hidraw6 (Connection: Bluetooth)
ago 03 22:55:31 gamer-linux ds4-translator[45396]: Successfully grabbed and hid event node: /dev/input/event257
ago 03 22:55:31 gamer-linux ds4-translator[45396]: Successfully grabbed and hid event node: /dev/input/event31
ago 03 22:55:31 gamer-linux ds4-translator[45396]: Successfully hid joystick node: /dev/input/js1
ago 03 22:55:31 gamer-linux ds4-translator[45396]: Successfully grabbed and hid event node: /dev/input/event256
ago 03 22:55:31 gamer-linux ds4-translator[45396]: Virtual USB Controller created.
ago 03 22:55:31 gamer-linux ds4-translator[45396]: UHID Device Started by kernel
ago 03 22:55:31 gamer-linux ds4-translator[45396]: UHID Device Opened by host
```
- make it so the builded files are organized in build/
