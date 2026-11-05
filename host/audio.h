// Host sound output: plays the game's mixed audio frames through WASAPI.
#pragma once
#include <cstdint>

// Call from one thread only, the one that runs the game's audio callback.
int  audio_out_room();                                  // whole game frames the device wants now; -1 when there is no device, so the caller keeps its own clock
void audio_out_submit(const uint8_t* frame);            // one game frame: 256 big-endian floats for each of 6 channels, one channel after another
