// The two upgrade cycle actions: which pad input steps the active upgrade and which steps the passive one, the way RB steps weapons.
// The game itself uses neither default input (LT, right stick click). Both can be rebound in the Controls menu.
#pragma once
#include <cstdint>
#include <string>

// Which input triggers slot 0 (active) or slot 1 (passive): a name from the settings file (a b x y lb rb lt rt back start l3 r3 up down left right none).
void upgrades_set_source(int slot, const char* name);

// From the controller read, once per state read: the buttons word and the trigger values. Notes a press of either source, on the press and not while held.
void upgrades_poll_pad(uint16_t buttons, uint8_t lt, uint8_t rt);

// On the game's Lua thread: true if a press is waiting, and the Lua that carries it out (clears what was waiting).
bool upgrades_pending();
std::string upgrades_cycle_lua();

// Also used by tests that simulate a press.
void upgrades_note_press(int slot);
