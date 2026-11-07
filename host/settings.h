// Player-facing settings: bcr2.ini beside the saves folder, and the bridge to the in-game PC Settings menu.
#pragma once
#include <string>

// Reads bcr2.ini and turns what it holds into the BCR2_* switches the renderer reads at start-up. A switch already set in the environment wins, so tests can still override.
// Must run before the renderer's start-up code reads the environment.
void settings_apply_to_env();

// The Lua that builds the PC Settings menu in the game's own state, and the small poll that collects changes. Both run on the game's Lua thread.
std::string settings_install_lua();
std::string settings_poll_lua();

// What the menu queued, as "key=value;key=value". Applies each change and saves the file.
void settings_apply_from_lua(const std::string& changes);
