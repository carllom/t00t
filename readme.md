# T00T - RP2350 audio excursions

This is an experiment in digital synthesis methods and algorithms using RP2350 Raspberry Pi Pico 2.

## Development board 

For ease of use and flexibility (and because I have it) I am using the Pimoroni Pico Demo card https://pimoroni.com/picovgademo.

The card has a PCM5100A DAC for line out audio over I2S. It also has an amplifier+jack connected to a PWM output.

Sample code in:

- https://github.com/raspberrypi/pico-extras
- https://github.com/raspberrypi/pico-playground

## Strategy

The aim is to create a good foundation for tone generation and to investigate sound generation and hierarchies from a historical perspective. Both by doing approximations of classic sound chips and digital music equipment based on the general principles of working, not the actual implementation.

The end goal is not necessarily audio quality, but instead efficient and simple real-time audio generation.
