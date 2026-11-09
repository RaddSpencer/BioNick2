#include "kbm.h"
#include <windows.h>
#include <atomic>
#include <cstring>

namespace {

struct Key { const char* name; int vk; const char* icon; bool assignable; };
const Key kKeys[] = {
    { "key_a", 'A', "keyboard_a", false }, { "key_b", 'B', "keyboard_b", true }, { "key_c", 'C', "keyboard_c", true }, { "key_d", 'D', "keyboard_d", false },
    { "key_e", 'E', "keyboard_e", true }, { "key_f", 'F', "keyboard_f", true }, { "key_g", 'G', "keyboard_g", true }, { "key_h", 'H', "keyboard_h", true },
    { "key_i", 'I', "keyboard_i", true }, { "key_j", 'J', "keyboard_j", true }, { "key_k", 'K', "keyboard_k", true }, { "key_l", 'L', "keyboard_l", true },
    { "key_m", 'M', "keyboard_m", true }, { "key_n", 'N', "keyboard_n", true }, { "key_o", 'O', "keyboard_o", true }, { "key_p", 'P', "keyboard_p", true },
    { "key_q", 'Q', "keyboard_q", true }, { "key_r", 'R', "keyboard_r", true }, { "key_s", 'S', "keyboard_s", false }, { "key_t", 'T', "keyboard_t", true },
    { "key_u", 'U', "keyboard_u", true }, { "key_v", 'V', "keyboard_v", true }, { "key_w", 'W', "keyboard_w", false }, { "key_x", 'X', "keyboard_x", true },
    { "key_y", 'Y', "keyboard_y", true }, { "key_z", 'Z', "keyboard_z", true },
    { "key_0", '0', "keyboard_0", true }, { "key_1", '1', "keyboard_1", true }, { "key_2", '2', "keyboard_2", true }, { "key_3", '3', "keyboard_3", true },
    { "key_4", '4', "keyboard_4", true }, { "key_5", '5', "keyboard_5", true }, { "key_6", '6', "keyboard_6", true }, { "key_7", '7', "keyboard_7", true },
    { "key_8", '8', "keyboard_8", true }, { "key_9", '9', "keyboard_9", true },
    { "key_space", VK_SPACE, "keyboard_space", true },
    { "key_lshift", VK_LSHIFT, "keyboard_shift_left", true }, { "key_rshift", VK_RSHIFT, "keyboard_shift_right", true },
    { "key_lctrl", VK_LCONTROL, "keyboard_ctrl_left", true }, { "key_rctrl", VK_RCONTROL, "keyboard_ctrl_right", true },
    { "key_lalt", VK_LMENU, "keyboard_alt_left", true }, { "key_ralt", VK_RMENU, "keyboard_alt_right", true },
    { "key_num0", VK_NUMPAD0, "keyboard_numpad_0", true }, { "key_num1", VK_NUMPAD1, "keyboard_numpad_1", true }, { "key_num2", VK_NUMPAD2, "keyboard_numpad_2", true },
    { "key_num3", VK_NUMPAD3, "keyboard_numpad_3", true }, { "key_num4", VK_NUMPAD4, "keyboard_numpad_4", true }, { "key_num5", VK_NUMPAD5, "keyboard_numpad_5", true },
    { "key_num6", VK_NUMPAD6, "keyboard_numpad_6", true }, { "key_num7", VK_NUMPAD7, "keyboard_numpad_7", true }, { "key_num8", VK_NUMPAD8, "keyboard_numpad_8", true },
    { "key_num9", VK_NUMPAD9, "keyboard_numpad_9", true }, { "key_num_add", VK_ADD, "keyboard_numpad_plus", true },
    { "key_delete", VK_DELETE, "keyboard_delete", true }, { "key_end", VK_END, "keyboard_end", true }, { "key_home", VK_HOME, "keyboard_home", true },
    { "key_pgup", VK_PRIOR, "keyboard_page_up", true }, { "key_pgdn", VK_NEXT, "keyboard_page_down", true }, { "key_insert", VK_INSERT, "keyboard_insert", true },
    { "key_tab", VK_TAB, "keyboard_tab", false }, { "key_escape", VK_ESCAPE, "keyboard_escape", false },
    { "key_comma", VK_OEM_COMMA, "keyboard_comma", true }, { "key_period", VK_OEM_PERIOD, "keyboard_period", true },
    { "key_semicolon", VK_OEM_1, "keyboard_semicolon", true }, { "key_apostrophe", VK_OEM_7, "keyboard_apostrophe", true },
    { "key_slash", VK_OEM_2, "keyboard_slash_forward", true }, { "key_backslash", VK_OEM_5, "keyboard_slash_back", true },
    { "key_minus", VK_OEM_MINUS, "keyboard_minus", true }, { "key_equals", VK_OEM_PLUS, "keyboard_equals", true },
    { "key_bracket_open", VK_OEM_4, "keyboard_bracket_open", true }, { "key_bracket_close", VK_OEM_6, "keyboard_bracket_close", true },
    { "mouse_left", VK_LBUTTON, "mouse_left", true }, { "mouse_right", VK_RBUTTON, "mouse_right", true }, { "mouse_middle", VK_MBUTTON, "mouse_scroll", true },
    { "mouse_x1", VK_XBUTTON1, "mouse_side_back", true }, { "mouse_x2", VK_XBUTTON2, "mouse_side_forward", true },
    { "mouse_wheel_up", 0, "mouse_scroll_up", true }, { "mouse_wheel_down", 0, "mouse_scroll_down", true },
};
const int kNumKeys = (int)(sizeof kKeys / sizeof kKeys[0]);

std::atomic<unsigned long long> g_wheelUp{0}, g_wheelDown{0};     // GetTickCount64 until which the wheel counts as pressed
bool g_prev[sizeof kKeys / sizeof kKeys[0]] = {};

const Key* find(const std::string& n) { for (const Key& k : kKeys) if (n == k.name) return &k; return nullptr; }
// Which keys and buttons are down, kept from the window's own key and mouse messages (not read from the system-wide keyboard state).
std::atomic<unsigned char> g_down[256];
bool raw_down(const Key& k) {
    if (k.vk == 0) { const unsigned long long now = GetTickCount64(); return k.name[12] == 'u' ? now < g_wheelUp.load() : now < g_wheelDown.load(); }
    return g_down[k.vk & 0xFF].load() != 0;
}

}  // namespace

bool kbm_window_active() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
bool kbm_known(const std::string& name) { const Key* k = find(name); return k && k->assignable; }
bool kbm_down(const std::string& name) { const Key* k = find(name); return k && kbm_window_active() && raw_down(*k); }
const char* kbm_icon(const std::string& name) { const Key* k = find(name); return k ? k->icon : nullptr; }
void kbm_note_wheel(int delta) {
    const unsigned long long until = GetTickCount64() + 150;
    if (delta > 0) g_wheelUp = until; else if (delta < 0) g_wheelDown = until;
}
bool kbm_reserved_down(int vk) { return kbm_window_active() && g_down[vk & 0xFF].load() != 0; }
// Called from the window procedure for every message: keeps the table of held keys and mouse buttons.
void kbm_note_message(void* hwnd, unsigned msg, unsigned long long w, long long l) {
    auto set = [](int vk, bool on) { g_down[vk & 0xFF] = on ? 1 : 0; };
    switch (msg) {
      case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        const bool on = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        int vk = (int)w; const bool ext = (l >> 24) & 1; const int scan = (int)((l >> 16) & 0xFF);
        if (vk == VK_SHIFT) { set(VK_SHIFT, on); vk = scan == 0x36 ? VK_RSHIFT : VK_LSHIFT; }
        else if (vk == VK_CONTROL) { set(VK_CONTROL, on); vk = ext ? VK_RCONTROL : VK_LCONTROL; }
        else if (vk == VK_MENU) { set(VK_MENU, on); vk = ext ? VK_RMENU : VK_LMENU; }
        set(vk, on);
        break; }
      case WM_LBUTTONDOWN: case WM_LBUTTONUP: set(VK_LBUTTON, msg == WM_LBUTTONDOWN); goto mouse;
      case WM_RBUTTONDOWN: case WM_RBUTTONUP: set(VK_RBUTTON, msg == WM_RBUTTONDOWN); goto mouse;
      case WM_MBUTTONDOWN: case WM_MBUTTONUP: set(VK_MBUTTON, msg == WM_MBUTTONDOWN); goto mouse;
      case WM_XBUTTONDOWN: case WM_XBUTTONUP: set(HIWORD(w) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2, msg == WM_XBUTTONDOWN); goto mouse;
      case WM_KILLFOCUS: case WM_CAPTURECHANGED: if (msg == WM_KILLFOCUS) for (auto& d : g_down) d = 0; break;
      case WM_ACTIVATEAPP: if (!w) for (auto& d : g_down) d = 0; break;
    }
    return;
mouse:   // keep receiving the button-up when the pointer has left the window
    if (g_down[VK_LBUTTON] || g_down[VK_RBUTTON] || g_down[VK_MBUTTON] || g_down[VK_XBUTTON1] || g_down[VK_XBUTTON2]) { if (GetCapture() != (HWND)hwnd) SetCapture((HWND)hwnd); }
    else if (GetCapture() == (HWND)hwnd) ReleaseCapture();
}
void kbm_forget_edges() { for (int i = 0; i < kNumKeys; i++) g_prev[i] = kbm_window_active() && raw_down(kKeys[i]); }
std::string kbm_new_press() {
    const bool active = kbm_window_active();
    std::string found;
    for (int i = 0; i < kNumKeys; i++) {
        const bool d = active && raw_down(kKeys[i]);
        if (d && !g_prev[i] && kKeys[i].assignable && found.empty()) found = kKeys[i].name;
        g_prev[i] = d;
    }
    return found;
}
