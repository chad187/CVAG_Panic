example secret: 
WIFI_PROFILES='{"SSID1", "pw1"}, {"ssid2", "pw2"}'
""\"htts://googleaddress/exec\"""
""\"password\"""


The 2N7000 Pinout (Flat side facing you)Left 
Pin (1): Source(L) Connect to T-Display GND Middle 
Pin (2): Gate(M) Connect to T-Display GPIO 26 Right 
Pin (3): Drain(R) Connect to Arcade LED (-)
Where the 4.7kΩ Resistor Goes:Connect one leg of the resistor to the Middle Pin (Gate).
Connect the other leg of the resistor to the Left Pin (Source / GND).
Connect the 5v to the Arcade LED (+)


Left Pin: Hooked up to Ground. It also holds one leg of your resistor.

Middle Pin: Hooked up to your control wire (GPIO 26). It holds the other leg of your resistor.

Right Pin: Hooked up to the negative wire of your LED.