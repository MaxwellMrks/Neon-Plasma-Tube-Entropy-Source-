# Neon-Plasma-Tube-Entropy-Source-
Using the Ghs-2 Soviet noise tube I created a scientific instrument in order to study ionization in neon. 
This instrument doubles as a random number generator, AC voltage fluctuations are decoupled from the anode side of the tube, biased, then fed into ADC pin of an ESP32. The mV voltage fluctuations are caused by a combination of shot noise, ionization events, and other factors. More on that below. 

# The GSH-2 

The GSH-2 is a 1970s broadband noise tube manufactured in the USSR, these were used predominatnely for military purposes, namely random number generation. 
The glass envelope , is 14 inches long , 5mm diameter at the thinnest point, and 50 mm in diamater at the thickest.  
This tube is optimized for 7 - 10Ghz signals, and was used mostely in radar and microwave equipqment. 
In this project I sampled only the low-end of the spectrum, 0-10khz, making this a relativily doable task for any hobbiest, without RF equipment. 


There are 7 pins, making it a typical octogonal vacuum-tube style base. Pin 7 is NC. 
From looking at the gap, moving clockwise from the left pin of the gap to right, count 1-7.

Pins 3-5 are connected internally at a near short (.5 ohms)
Pins 1-6 are also connected interally at a near short (.5 ohms) 
pin 7 is NC 
Pin 4 is connected to the sleave 


# Instrument Overview
This project overview will be divided into the cathode and anode sides of the tube respectivily and the engineering involved in each. 

# Cathode
For this project I used a PSU from a Dell PC I salvaged, 12V with the capability to supply 16A. However any supply meeting 12V, at least able to supply 6A would suffice. 



