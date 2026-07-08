# Neon-Plasma-Tube-Entropy-Source-
Using the Ghs-2 Soviet noise tube I created a scientific instrument in order to study ionization in neon. 
This instrument doubles as a random number generator, AC voltage fluctuations are decoupled from the anode side of the tube 
, biased, then fed into ADC pin of an ESP32.
The mV voltage fluctuations are caused by a combination of shot noise, ionization events, and other factors. More on that below. 

# The GSH-2 

The GSH-2 is a 1970s broadband noise tube manufactured in the USSR, these were used predominatnely for military purposes, namely random number generation. 

# Instrument Overview
This project overview will be divided into the cathode and anode sides of the tube respectivily and the engineering involved in each. 

# Cathode
For this project I used a PSU from a Dell PC I salvaged, 12V with the capability to supply 16A. However any supply meeting 12V, at least able to supply 6A would suffice. 



