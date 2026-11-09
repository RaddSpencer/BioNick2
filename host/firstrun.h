// First-run setup: see firstrun.cpp.
#pragma once
#include <string>

// The folder the unpacked game lives in (host\game, beside the build folder).
std::string game_dir();
bool game_is_unpacked(const std::string& gameDir);

// Fills in the two paths the host needs. If the game is not unpacked yet, runs the setup dialogs first.
// Returns false when the player cancelled, or chose not to start now: the program should then simply exit.
bool first_run_setup(std::string& basefile, std::string& contentRoot);
