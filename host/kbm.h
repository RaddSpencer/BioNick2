// Keyboard and mouse as game input. Each action (jump, fire, ...) can have a key or mouse button as well as a pad button; while the game is running a pressed key
// presses that action's pad input in the state handed to the game. Menus get the arrow keys, Enter and Escape. See settings.cpp for how it is wired in.
#pragma once
#include <string>
#include <vector>

// Names: "key_a" ... "key_z", "key_0" ... "key_9", "key_space", "key_lshift", "mouse_left", "mouse_wheel_up" and so on.
bool kbm_known(const std::string& name);          // a name this module knows, and one an action may be given
bool kbm_down(const std::string& name);           // pressed now (false when the game window is not in front)
std::string kbm_new_press();                      // an assignable key or button pressed since the last call, or empty
const char* kbm_icon(const std::string& name);    // icon file (no extension) in assets\keys, or nullptr
void kbm_note_wheel(int delta);                   // from the window procedure
bool kbm_reserved_down(int vk);                   // a fixed key (arrows, Enter, Escape ...) pressed now, window in front
bool kbm_window_active();
void kbm_forget_edges();                          // the next kbm_new_press ignores what is held now

// A picture drawn over the frame, in the 1280x720 screen space the game's menus use. Implemented in gpu.cpp.
struct GpuSprite { std::string icon; float x, y, w, h; };
void gpu_set_sprites(const std::vector<GpuSprite>& sprites);
