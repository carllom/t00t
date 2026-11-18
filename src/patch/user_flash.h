#pragma once

#include "user_bank.h"

// The user-bank region on the device: the last kUserRegionSize bytes of
// flash, read through XIP. A sector write parks Core 1, silences audio and
// masks every Core 0 interrupt except the audio DMA's for the erase and
// program. The base is null if the firmware image reaches into the region.
UserFlash user_flash_device();
