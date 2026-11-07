// LT and a click of the right stick cycle the equipped arm upgrades, the way RB cycles weapons. The game doesn't use either input.
#pragma once
#include <string>

// From the controller read: 0 = LT (active upgrade), 1 = right stick click (passive upgrade). Called on the press, not while held.
void upgrades_note_press(int slot);

// On the game's Lua thread: true if a press is waiting, and the Lua that carries it out (clears what was waiting).
bool upgrades_pending();
std::string upgrades_cycle_lua();
