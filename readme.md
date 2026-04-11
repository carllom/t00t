# T00T an experiment in digital synthesis methods and algorithms using Raspberry Pi Pico

## Development board 

For ease of use and flexibility (and because I have it) I will be using the Pimoroni Pico Demo card https://pimoroni.com/picovgademo.

The card has a PCM5100A DAC for line out audio over I2S. It also has an amplifier+jack connected to a PWM output.

Sample code in:

- https://github.com/raspberrypi/pico-extras
- https://github.com/raspberrypi/pico-playground

## Strategy

The aim is to create a good foundation for tone generation and to investigate sound generation and hierarchies from a historical perspective. Both by doing approximations of classic sound chips and digital music equipment based on the general principles of working, not the actual implementation.

But we start simple. First setting up a workflow for generating firmware for the raspberry pi pico, then doing a simple generated waveform to verify output.

We work in c++