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
Pins 2 and 4 are connected to the sleave of the filament. 

# The tube works like this
Pins 1-6 (filament) is heated at 12V 1.08A (DC) and turns red hot, the filament is covered in an oxide layer which when heated boils off electrons via thermionic emission.
Simultaneous to this, 10Kvac 200-500Khz (RF) strike voltage is supplied to the tube for a brief moment, the ionizing neon triggers the logic circuit (more on below) which turns off the strike voltage, and turns on a sustaining voltage of 150-200Vdc. Once the ionization is sustained, there will be a negative dark space observable , there will be an ionization cloud surrounding the cathode side of the tube. 
Whith the filament boiling off electrons , and +150-200Vdc supplied to the anode , electrons from the filament (thermionic emission) with fly through the dark space of the tube , cuasing voltage fluctuations on the anode, these minute mVAC fluctuations are decoupled, biased, and fed into the ADC GPIO P35 of an ESP32. 


# Instrument Overview
This project overview will be divided into the cathode and anode sides of the tube respectivily and the engineering involved in each. 

# Cathode
For this project I used a PSU from a Dell PC I salvaged, 12V with the capability to supply 16A. However any supply meeting 12V, at least able to supply 6A would suffice. 

# Anode 

# Mistakes I made 

