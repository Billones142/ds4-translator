- implement system to detect specific programs (native or wine) and apply a controller config based on that
- the program should be able to "inject" the controller to programs
- make it so the builded files are organized in build/
- add featrure to make passthough of batery status
- kerner messages:
´´´
stefano@gamer-linux:~$ journalctl -k -r
Journal file /var/log/journal/6503f1a4984e4725abb0e8938c245cbd/system@0006583710e23044-a83fec512e8fa81b.journal~ is truncated, ignoring file.
ago 04 21:06:54 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:54 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:53 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:53 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:52 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:52 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:51 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:51 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:50 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:50 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:49 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:49 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:48 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:48 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:47 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:47 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:46 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:46 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:45 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:45 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:44 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:44 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:43 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:43 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:42 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:42 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:41 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:41 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:40 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:40 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:39 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 21:06:39 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 20:50:23 gamer-linux kernel: playstation 0003:054C:0CE6.0021: Registered DualSense controller hw_>
ago 04 20:50:23 gamer-linux kernel: input: Sony Interactive Entertainment DualSense Wireless Controller >
ago 04 20:50:23 gamer-linux kernel: input: Sony Interactive Entertainment DualSense Wireless Controller >
ago 04 20:50:23 gamer-linux kernel: input: Sony Interactive Entertainment DualSense Wireless Controller >
ago 04 20:50:23 gamer-linux kernel: input: Sony Interactive Entertainment DualSense Wireless Controller >
ago 04 20:50:23 gamer-linux kernel: playstation 0003:054C:0CE6.0021: hidraw7: USB HID v81.11 Gamepad [So>
ago 04 20:50:22 gamer-linux kernel: playstation 0005:054C:05C4.0020: Registered DualShock4 controller hw>
ago 04 20:50:22 gamer-linux kernel: input: Wireless Controller Touchpad as /devices/virtual/misc/uhid/00>
ago 04 20:50:22 gamer-linux kernel: input: Wireless Controller Motion Sensors as /devices/virtual/misc/u>
ago 04 20:50:22 gamer-linux kernel: input: Wireless Controller as /devices/virtual/misc/uhid/0005:054C:0>
ago 04 20:50:22 gamer-linux kernel: playstation 0005:054C:05C4.0020: hidraw6: BLUETOOTH HID v1.00 Gamepa>
ago 04 20:50:22 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 20:50:22 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 20:50:21 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 20:50:20 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 20:50:19 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 20:50:19 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full
ago 04 20:50:18 gamer-linux kernel: hid-generic 0003:12BA:0050.0006: Output queue is full

´´´
- make feature to disconect BT controller after some ammount of inactivity
