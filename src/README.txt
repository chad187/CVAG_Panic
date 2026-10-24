example secret: 
WIFI_PROFILES='{"SSID1", "pw1"}, {"ssid2", "pw2"}'
""\"htts://googleaddress/exec\"""
""\"password\"""


Arcade button: connect one side to GPIO 26 and the other side to GND.
The pin uses the chip's internal pull-up, so a press reads low.

The 2N7000 Pinout (Flat side facing you)
Left Pin (1): Source. Connect to T-Display GND.
Middle Pin (2): Gate. Connect to T-Display GPIO 27.
Right Pin (3): Drain. Connect to Arcade LED (-).

Where the 4.7k resistor goes: one leg on the middle pin (gate), the other leg on the left pin (source / GND).
Connect 5V to the arcade LED (+).
