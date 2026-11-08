// Settings file and the in-game PC Settings and Controls menus.
//
// The menus replace the Achievements entry of the game's main and pause menus (nothing in this build can show achievements) and the Controls entry of
// Help & Options (a picture of the controller). They are built at run time, in Lua, from the same node and item classes the game's menu files use, so the
// game assets stay as they are. A menu only talks to the host through one Lua global: each change appends "key=value" to Global.bcr_pc_out, and the host
// collects that a few times a second (hooks.cpp) and applies it here.
//
// Settings the renderer reads once at start-up (render scale, variable refresh) are saved and take effect on the next start. The display mode and the
// button prompts apply at once. Button bindings are applied by the Lua itself, in the game's input layer (see kInstallLua), except the two upgrade cycle
// buttons, which the host reads from the pad (upgradecycle.cpp).
#include "settings.h"
#include "upgradecycle.h"
#include "kbm.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <mutex>

int gpu_display_mode();
void gpu_set_display_mode(int mode);
void gpu_set_button_set(int s);
void gpu_set_gamma(int percent);
void gpu_set_gamma_preview(bool on);
std::string gpu_fps_text();
void gpu_set_texfilter(int mode);
void gpu_set_aa(int mode);

namespace {

// Function statics: settings_apply_to_env runs from another file's static initialiser, possibly before this file's globals would exist.
std::mutex& lock_() { static std::mutex m; return m; }
std::map<std::string, std::string>& values_() { static std::map<std::string, std::string> v; return v; }

#define BCR_SOURCES "a", "b", "x", "y", "lb", "rb", "lt", "rt", "l3", "r3"
struct Def { const char* key; const char* def; const char* allowed[20]; const char* env; };
const Def kDefs[] = {
    { "display", "window", { "window", "borderless", "fullscreen", nullptr }, "BCR2_DISPLAY" },
    { "scale",   "auto",   { "auto", "1", "2", "3", "4", nullptr },           "BCR2_RESSCALE" },
    { "outres",  "auto",   { "auto", "720", "1080", "1440", "2160", nullptr }, "BCR2_OUTRES" },
    { "vsync",   "on",     { "on", "off", nullptr },                          "BCR2_VRR" },          // off = frames shown at fixed times after the game tick (variable refresh pacing)
    { "filter",  "default", { "default", "bilinear", "trilinear", "aniso2", "aniso4", "aniso8", "aniso16", nullptr }, nullptr },
    { "aa",      "off",    { "off", "fxaa", "fxaahigh", nullptr },            nullptr },
    { "fps",     "off",    { "off", "on", nullptr },                          nullptr },
    { "gamma",   "100",    { nullptr },                                       nullptr },             // percent, 50 to 150: checked by number
    { "buttons", "xbox",    { "xbox", "playstation", "switch", nullptr },     nullptr },
    { "nojump",  "off",     { "off", "on", nullptr },                          nullptr },
    { "god",     "off",     { "off", "on", nullptr },                          nullptr },
    { "swapconfirm", "off", { "off", "on", nullptr },                          nullptr },
    // Which physical input triggers each action. The defaults are the game's own layout.
    { "b_jump",      "a",     { BCR_SOURCES, nullptr }, nullptr },
    { "b_arm",       "b",     { BCR_SOURCES, nullptr }, nullptr },
    { "b_fire",      "x",     { BCR_SOURCES, nullptr }, nullptr },
    { "b_upgrade",   "y",     { BCR_SOURCES, nullptr }, nullptr },
    { "b_next",      "rb",    { BCR_SOURCES, nullptr }, nullptr },
    { "b_prev",      "lb",    { BCR_SOURCES, nullptr }, nullptr },
    { "b_cycleact",  "lt",    { BCR_SOURCES, nullptr }, nullptr },
    { "b_biovision", "rt",    { BCR_SOURCES, nullptr }, nullptr },
    { "b_cyclepas",  "r3",    { BCR_SOURCES, nullptr }, nullptr },
    // The key or mouse button for each action, as well as the pad button above. Checked by kbm_known.
    { "k_jump",      "key_space",       { nullptr }, nullptr },
    { "k_arm",       "mouse_right",     { nullptr }, nullptr },
    { "k_fire",      "mouse_left",      { nullptr }, nullptr },
    { "k_upgrade",   "key_e",           { nullptr }, nullptr },
    { "k_next",      "mouse_wheel_up",  { nullptr }, nullptr },
    { "k_prev",      "mouse_wheel_down", { nullptr }, nullptr },
    { "k_cycleact",  "key_q",           { nullptr }, nullptr },
    { "k_biovision", "key_f",           { nullptr }, nullptr },
    { "k_cyclepas",  "key_c",           { nullptr }, nullptr },
};

std::string ini_path() {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string s = exe; size_t p = s.find_last_of('\\');
    return (p == std::string::npos ? std::string(".") : s.substr(0, p)) + "\\..\\bcr2.ini";
}

const Def* find_def(const std::string& key) { for (const Def& d : kDefs) if (key == d.key) return &d; return nullptr; }
bool allowed(const Def& d, const std::string& v) {
    if (d.key[0] == 'k' && d.key[1] == '_') return kbm_known(v);
    if (!strcmp(d.key, "gamma")) { char* end = nullptr; long n = strtol(v.c_str(), &end, 10); return end && !*end && !v.empty() && n >= 50 && n <= 150; }
    for (const char* a : d.allowed) if (a && v == a) return true;
    return false;
}

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void load_ini() {
    for (const Def& d : kDefs) values_()[d.key] = d.def;
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            std::string l = trim(line);
            if (l.empty() || l[0] == '#' || l[0] == ';') continue;
            size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
            if (k == "buttons" && v == "classic") v = "xbox";          // the old "classic" prompts are gone
            if (k == "vrr") { if (v == "on") values_()["vsync"] = "off"; continue; }   // the old variable refresh switch is now Vsync off
            const Def* d = find_def(k);
            if (d && allowed(*d, v)) values_()[k] = v;
            else fprintf(stderr, "[settings] ignoring '%s' in bcr2.ini\n", l.c_str());
        }
        fclose(f);
    }
}

void save_ini() {
    if (FILE* f = fopen(ini_path().c_str(), "w")) {
        fprintf(f, "# Bionic Commando Rearmed 2 (recomp) settings. Written by the PC Settings and Controls menus; you can edit it by hand while the game is closed.\n");
        fprintf(f, "# display: window, borderless or fullscreen.  scale: auto, 1, 2, 3 or 4 times 1280x720.  vsync: on, or off to show frames at fixed times after the game tick (best on a G-Sync or FreeSync display, else it tears).  buttons: xbox, playstation or switch (the button labels).\n");
        fprintf(f, "# scale: internal render size (1 = 720p, 2 = 1440p, 3 = 2160p).  outres: the size of the picture on screen, auto or 720, 1080, 1440, 2160 (the render is resampled to it).  gamma: percent, 50 to 150.\n");
        fprintf(f, "# nojump: on turns jumping off (the Retro rule: the Death From Above move still works).  god: on makes the player invulnerable with unlimited lives.\n");
        fprintf(f, "# swapconfirm: on makes the right face button (Xbox B) confirm and the bottom one (Xbox A) cancel in the menus.\n");
        fprintf(f, "# b_*: the pad input that triggers each action: a b x y lb rb lt rt l3 r3.  k_*: its key or mouse button (key_a, key_space, key_lshift, mouse_left, mouse_wheel_up, ...).\n");
        for (const Def& d : kDefs) fprintf(f, "%s=%s\n", d.key, values_()[d.key].c_str());
        fclose(f);
    }
}

int filter_from_name(const std::string& v) { static const char* n[] = { "default", "bilinear", "trilinear", "aniso2", "aniso4", "aniso8", "aniso16" }; for (int i = 0; i < 7; i++) if (v == n[i]) return i; return 0; }
int aa_from_name(const std::string& v) { return v == "fxaa" ? 1 : v == "fxaahigh" ? 2 : 0; }
int button_set_from_name(const std::string& v) { return v == "playstation" ? 2 : v == "switch" ? 3 : 1; }

// Press-to-remap state. The menu poll says which action row is selected; the pad filter records the first assignable button pressed while one is.
std::string& cap_action_() { static std::string s; return s; }
std::string& cap_pending_() { static std::string s; return s; }
bool& cap_fresh_() { static bool b = false; return b; }
uint32_t& cap_prev_() { static uint32_t m = 0; return m; }
bool& ui_open_() { static bool b = false; return b; }
bool& cap_block_() { static bool b = false; return b; }
int display_from_name(const std::string& v) { return v == "borderless" ? 1 : v == "fullscreen" ? 2 : 0; }
const char* display_name(int m) { return m == 1 ? "borderless" : m == 2 ? "fullscreen" : "window"; }

// The PC Settings and Controls menus, in Lua. @INIT@ becomes a table of the current values. No -- comments: this text is also handed to the game as one line while testing.
//
// Bindings work on the game's named inputs. Each gameplay action ("jump", "rope" for the bionic arm, "fire", ...) has a connection in the controller setup with an
// _input_name_list of physical inputs ("a", "left_shoulder", ...). Menus use other names ("confirm", "cancel", "menu_*"), so rebinding or unbinding a gameplay
// action leaves menu navigation and text boxes alone. The list is rewritten from the poll (the player and its controller are recreated for each level).
const char* kInstallLua = R"LUA(

local INIT = @INIT@
if not (managers and managers.menu and managers.menu._registered_menus and managers.localization and Global) then return "wait: managers" end
local reg = managers.menu._registered_menus
if not (managers.music and managers.music.music_events) then return "wait: music" end
local main, pause = reg.menu_main, reg.menu_pause
if not (main and main.data and main.data._nodes and main.data._nodes.main) then return "wait: menu_main" end
local MenuNode = getmetatable(main.data._nodes.main)
if not (MenuNode and MenuNode.new) then return "wait: node class" end
local STR = {
  bcr_pc_settings = "Graphics Settings",
  bcr_pc_gamma = "Gamma Correction",
  bcr_pc_vsync = "Vsync (requires restart)",
  bcr_pc_vsync_off = "Off (Variable Refresh)",
  bcr_hint_start = "Start",
  bcr_hint_stop = "Stop",
  bcr_music_title = "Music Test",
  bcr_music_track = "Track",
  bcr_pc_filter = "Texture Filtering",
  bcr_pc_filter_default = "Game Default",
  bcr_pc_filter_bilinear = "Bilinear",
  bcr_pc_filter_trilinear = "Trilinear",
  bcr_pc_filter_aniso2 = "Anisotropic 2x",
  bcr_pc_filter_aniso4 = "Anisotropic 4x",
  bcr_pc_filter_aniso8 = "Anisotropic 8x",
  bcr_pc_filter_aniso16 = "Anisotropic 16x",
  bcr_pc_aa = "Antialiasing",
  bcr_pc_aa_off = "Off",
  bcr_pc_aa_fxaa = "FXAA",
  bcr_pc_aa_fxaahigh = "FXAA (High)",
  bcr_pc_fps = "FPS Display",
  bcr_pc_outres = "Render Resolution (requires restart)",
  bcr_pc_outres_auto = "Auto",
  bcr_pc_outres_720 = "720p",
  bcr_pc_outres_1080 = "1080p",
  bcr_pc_outres_1440 = "1440p",
  bcr_pc_outres_2160 = "2160p",
  bcr_pc_display = "Display Mode",
  bcr_pc_display_window = "Windowed",
  bcr_pc_display_borderless = "Borderless Fullscreen",
  bcr_pc_display_fullscreen = "Exclusive Fullscreen",
  bcr_pc_scale = "Internal Resolution (requires restart)",
  bcr_pc_scale_auto = "Auto",
  bcr_pc_scale_1 = "720p",
  bcr_pc_scale_2 = "1440p",
  bcr_pc_scale_3 = "2160p",
  bcr_pc_buttons = "Button Labels",
  bcr_pc_nojump = "Disable Jump",
  bcr_pc_god = "God Mode",
  bcr_pc_swap = "Swap Confirm & Cancel",
  bcr_exit_q = "Exit without saving changes?",
  bcr_exit_cap = "Controls",
  bcr_restart_q = "Some changes need a restart. Save and restart now?",
  bcr_restart_cap = "Restart",
  bcr_yes = "Yes",
  bcr_no = "No",
  bcr_back = "Exit",
  bcr_blank = " ",
  bcr_pc_buttons_xbox = "Xbox",
  bcr_pc_buttons_playstation = "PlayStation",
  bcr_pc_buttons_switch = "Switch",
  bcr_pc_off = "Off",
  bcr_pc_on = "On",
  bcr_controls = "Controls",
  bcr_reset = "Reset to Defaults",
  bcr_act_jump = "Jump",
  bcr_act_arm = "Extend Bionic Arm",
  bcr_act_fire = "Fire",
  bcr_act_upgrade = "Use Active Upgrade",
  bcr_act_next = "Next Weapon",
  bcr_act_prev = "Previous Weapon",
  bcr_act_cycleact = "Cycle Active Upgrade",
  bcr_act_biovision = "Biovision",
  bcr_act_inventory = "Inventory",
  bcr_act_pause = "Pause Menu",
  bcr_act_cyclepas = "Cycle Passive Upgrade",
}
local LABELS = {
  xbox = { a = "A", b = "B", x = "X", y = "Y", lb = "LB", rb = "RB", lt = "LT", rt = "RT", back = "View", start = "Menu", l3 = "L-Stick Click", r3 = "R-Stick Click" },
  playstation = { a = "Cross", b = "Circle", x = "Square", y = "Triangle", lb = "L1", rb = "R1", lt = "L2", rt = "R2", back = "Create", start = "Options", l3 = "L3", r3 = "R3" },
  switch = { a = "B", b = "A", x = "Y", y = "X", lb = "L", rb = "R", lt = "ZL", rt = "ZR", back = "Minus", start = "Plus", l3 = "L-Stick Press", r3 = "R-Stick Press" },
}
local GLYPH = { a = 57344, b = 57345, x = 57346, y = 57347, back = 57348, start = 57349, l3 = 57350, r3 = 57351, lb = 57352, rb = 57353, lt = 57354, rt = 57355 }
local function set_labels(set)
  local t = LABELS[set] or LABELS.xbox
  for k, v in pairs(t) do STR["bcr_src_" .. k] = GLYPH[k] and utf8.char(GLYPH[k]) or v end
  STR.btn_x = utf8.char(GLYPH.x)
end
set_labels(INIT.buttons)
local L = managers.localization
if not rawget(L, "bcr_orig_text") then
  local orig = L.text
  L.bcr_orig_text = orig
  L.text = function(self, id, ...)
    local s = STR[id]
    if s then return s end
    return orig(self, id, ...)
  end
end
Global.bcr_pc = Global.bcr_pc or INIT
Global.bcr_set_labels = set_labels
local GFX = {
  { key = "display", text = "bcr_pc_display", options = { { "window", "bcr_pc_display_window" }, { "borderless", "bcr_pc_display_borderless" }, { "fullscreen", "bcr_pc_display_fullscreen" } } },
  { key = "scale", text = "bcr_pc_scale", options = { { "auto", "bcr_pc_scale_auto" }, { "1", "bcr_pc_scale_1" }, { "2", "bcr_pc_scale_2" }, { "3", "bcr_pc_scale_3" } } },
  { key = "outres", text = "bcr_pc_outres", options = { { "auto", "bcr_pc_outres_auto" }, { "720", "bcr_pc_outres_720" }, { "1080", "bcr_pc_outres_1080" }, { "1440", "bcr_pc_outres_1440" }, { "2160", "bcr_pc_outres_2160" } } },
  { key = "vsync", text = "bcr_pc_vsync", options = { { "on", "bcr_pc_on" }, { "off", "bcr_pc_vsync_off" } } },
  { key = "filter", text = "bcr_pc_filter", options = { { "default", "bcr_pc_filter_default" }, { "bilinear", "bcr_pc_filter_bilinear" }, { "trilinear", "bcr_pc_filter_trilinear" }, { "aniso2", "bcr_pc_filter_aniso2" }, { "aniso4", "bcr_pc_filter_aniso4" }, { "aniso8", "bcr_pc_filter_aniso8" }, { "aniso16", "bcr_pc_filter_aniso16" } } },
  { key = "aa", text = "bcr_pc_aa", options = { { "off", "bcr_pc_aa_off" }, { "fxaa", "bcr_pc_aa_fxaa" }, { "fxaahigh", "bcr_pc_aa_fxaahigh" } } },
  { key = "fps", text = "bcr_pc_fps", options = { { "off", "bcr_pc_off" }, { "on", "bcr_pc_on" } } },
}
local SETS = {
  { key = "nojump", text = "bcr_pc_nojump", options = { { "off", "bcr_pc_off" }, { "on", "bcr_pc_on" } } },
  { key = "god", text = "bcr_pc_god", options = { { "off", "bcr_pc_off" }, { "on", "bcr_pc_on" } } },
  { key = "buttons", text = "bcr_pc_buttons", main_only = true, options = { { "xbox", "bcr_pc_buttons_xbox" }, { "playstation", "bcr_pc_buttons_playstation" }, { "switch", "bcr_pc_buttons_switch" } } },
  { key = "swapconfirm", text = "bcr_pc_swap", main_only = true, options = { { "off", "bcr_pc_off" }, { "on", "bcr_pc_on" } } },
}
local SRC = { "a", "b", "x", "y", "lb", "rb", "lt", "rt", "l3", "r3" }
local PHYS = { a = "a", b = "b", x = "x", y = "y", lb = "left_shoulder", rb = "right_shoulder", lt = "left_trigger", rt = "right_trigger", back = "back", start = "start", l3 = "left_thumb", r3 = "right_thumb", up = "d_up", down = "d_down", left = "d_left", right = "d_right" }
local ACTIONS = {
  { key = "b_jump", text = "bcr_act_jump", def = "a", names = { "jump" } },
  { key = "b_arm", text = "bcr_act_arm", def = "b", names = { "rope" } },
  { key = "b_fire", text = "bcr_act_fire", def = "x", names = { "fire", "throw" } },
  { key = "b_upgrade", text = "bcr_act_upgrade", def = "y", names = { "active_arm_component" } },
  { key = "b_next", text = "bcr_act_next", def = "rb", names = { "switch_fwd" } },
  { key = "b_prev", text = "bcr_act_prev", def = "lb", names = { "switch_bwd" } },
  { key = "b_cycleact", text = "bcr_act_cycleact", def = "lt" },
  { key = "b_cyclepas", text = "bcr_act_cyclepas", def = "r3" },
  { key = "b_biovision", text = "bcr_act_biovision", def = "rt", names = { "activate_biovision" } },
}
Global.bcr_apply_binds = function()
  local mp = managers.player
  local pl = mp and mp.get_player and mp:get_player(1)
  if not (pl and alive(pl)) then return end
  local base = pl:base()
  local c = base and base._controller
  local cm = c and c._setup and c._setup._connection_map
  if not cm then return end
  local changed = false
  for _, a in ipairs(ACTIONS) do
    if a.names then
      local src = Global.bcr_pc[a.key] or a.def
      local want = PHYS[src]      for _, nm in ipairs(a.names) do
        local conn = cm[nm]
        if conn then
          local cur = conn._input_name_list or {}
          if want then
            if not (#cur == 1 and cur[1] == want) then conn._input_name_list = { want }; changed = true end
          elseif #cur > 0 then
            conn._input_name_list = {}
            changed = true
          end
        end
      end
    end
  end
  if changed and c.rebind_connections then c:rebind_connections() end
end
local function push(key, v)
  Global.bcr_pc_out = (Global.bcr_pc_out and (Global.bcr_pc_out .. ";") or "") .. key .. "=" .. tostring(v)
end
local function refresh(node)
  local cur = Global.bcr_pc
  for _, it in ipairs(node._items) do
    local key = it._parameters.bcr_key
    if key and cur[key] ~= nil then it:set_value(cur[key]) end
  end
  return node
end
local KDEF = { b_jump = "key_space", b_arm = "mouse_right", b_fire = "mouse_left", b_upgrade = "key_e", b_next = "mouse_wheel_up", b_prev = "mouse_wheel_down", b_cycleact = "key_q", b_biovision = "key_f", b_cyclepas = "key_c" }
local function kkey(key) return "k_" .. key:sub(3) end
Global.bcr_assign = function(key, src)
  local cur = Global.bcr_pc
  if src:find("^key_") or src:find("^mouse_") then
    local kk = kkey(key)
    local old = cur[kk]
    if old == src then return end
    for _, a in ipairs(ACTIONS) do
      local ak = kkey(a.key)
      if ak ~= kk and cur[ak] == src then
        cur[ak] = old
        push(ak, old)
      end
    end
    cur[kk] = src
    push(kk, src)
    for _, mname in ipairs({ "menu_main", "menu_pause" }) do
      local mm = reg[mname]
      local n = mm and mm.data and mm.data._nodes and mm.data._nodes.bcr_controls
      if n then refresh(n) end
    end
    return
  end
  local old = cur[key]
  if old == src then return end
  for _, a in ipairs(ACTIONS) do
    if a.key ~= key and cur[a.key] == src then
      cur[a.key] = old
      push(a.key, old)
    end
  end
  cur[key] = src
  push(key, src)
  for _, mname in ipairs({ "menu_main", "menu_pause" }) do
    local mm = reg[mname]
    local n = mm and mm.data and mm.data._nodes and mm.data._nodes.bcr_controls
    if n then refresh(n) end
  end
  Global.bcr_apply_binds()
end
Global.bcr_reset_defaults = function()
  for _, a in ipairs(ACTIONS) do
    Global.bcr_pc[a.key] = a.def
    push(a.key, a.def)
    local kk = kkey(a.key)
    Global.bcr_pc[kk] = KDEF[a.key]
    push(kk, KDEF[a.key])
  end
  for _, mname in ipairs({ "menu_main", "menu_pause" }) do
    local mm = reg[mname]
    local n = mm and mm.data and mm.data._nodes and mm.data._nodes.bcr_controls
    if n then refresh(n) end
  end
  Global.bcr_apply_binds()
end
local function toggle_item(key, text, options)
  local item = { _meta = "item", name = "bcr_" .. key, bcr_key = key, text_id = text, type = "CoreMenuItemToggle.ItemToggle" }
  for _, o in ipairs(options) do item[#item + 1] = { _meta = "option", value = o[1], text_id = o[2] } end
  return item
end
local RESTART_KEYS = { scale = true, outres = true, vsync = true }
local function wire_items(node)
  for _, it in ipairs(node._items) do
    local key = it._parameters.bcr_key
    if key then
      it._parameters.callback = { function(self)
        local v = self:value()
        if key:sub(1, 2) == "b_" then
          Global.bcr_assign(key, v)
          return
        end
        Global.bcr_pc[key] = v
        if key == "buttons" and Global.bcr_set_labels then Global.bcr_set_labels(v) end
        push(key, v)
        Global.bcr_apply_binds()
      end }
    elseif it._parameters.name == "bcr_back" then
      it._parameters.callback = { function()
        local om = managers.menu._open_menus
        local top = om and om[#om]
        if top and top.logic and top.logic.navigate_back then top.logic:navigate_back(true) end
      end }
    elseif it._parameters.name == "bcr_reset" then
      it._parameters.callback = { function() Global.bcr_reset_defaults() end }
    end
  end
end
)LUA"
R"LUA(
local function build(kind, menu, name, header, items, default_name, with_reset, straight, plain)
  local data = { _meta = "node", name = name }
  local flat = kind == "pause" or straight
  if straight and kind == "main" then data.camera_position = "left_outside" end
  if flat then data.header = header else data.header_id = header end
  for _, it in ipairs(items) do data[#data + 1] = it end
  if with_reset then data[#data + 1] = { _meta = "item", name = "bcr_reset", text_id = "bcr_reset" } end
  if straight and not plain then data[#data + 1] = { _meta = "item", name = "bcr_back", text_id = "bcr_back" } end
  data[#data + 1] = { _meta = "default_item", name = default_name }
  data[#data + 1] = { _meta = "node_type", name = flat and "MenuPauseNodeGui" or "Menu3dNodeGui" }
  local node = MenuNode:new(data)
  wire_items(node)
  node._parameters.modifier = { refresh }
  if menu.callback_handler then node:set_callback_handler(menu.callback_handler) end
  return node
end
local P = rawget(_G, "MenuPauseNodeGui")
if P and not rawget(P, "bcr_orig_init") then
  local orig = P.init
  P.bcr_orig_init = orig
  P.init = function(self, node, layer, parameters, ...)
    local np = node and node.parameters and node:parameters()
    if np and np.name == "bcr_controls" and type(parameters) == "table" then
      parameters.row_item_font = "fonts/merged_bcfont_44"
      parameters._stripe_panel_y = 0
    end
    if np and np.name == "bcr_music" and type(parameters) == "table" then parameters.row_item_font = "fonts/merged_bcfont_44" end
    local om0 = managers.menu and managers.menu._open_menus
    local in_pause = om0 and om0[#om0] and om0[#om0].name == "menu_pause" and np and np.name ~= "bcr_controls"
    if in_pause and type(parameters) == "table" then
      parameters._stripe_panel_y = 0
      parameters._stripe_panel_height = 720
    end
    local r = orig(self, node, layer, parameters, ...)
    if in_pause and self.item_panel and self._stripe_header then
      local n = 0
      for _ in ipairs(self.row_items or {}) do n = n + 1 end
      local top = math.max(20, (720 - (90 + n * 31)) / 2)
      self._stripe_header:set_y(top)
      self.item_panel:set_y(top + 90)
      if np.name == "bcr_gamma" and self._stripe_background then self._stripe_background:set_color(Color(1, 0, 0, 0)) end
    end
    if np and np.name == "bcr_music" and self.row_items then
      local y = 0
      for _, ri in ipairs(self.row_items) do
        local gp = ri.gui_panel
        if gp then
          gp:set_height(58)
          gp:set_y(y)
          y = y + 38
        end
      end
      if self.item_panel then self.item_panel:set_h(y + 60) end
    end
    if np and np.name == "bcr_controls" and self.item_panel and self.row_items then
      local y = 0
      for _, ri in ipairs(self.row_items) do
        local gp = ri.gui_panel
        if gp then
          if ri.name == "bcr_reset" then y = y + 38 end
          gp:set_height(58)
          gp:set_y(y)
          y = y + 38
        end
      end
      self.item_panel:set_h(y + 60)
      self.item_panel:set_y(105)
    end
    return r
  end
end
if managers.api and not rawget(managers.api, "bcr_rev") then
  managers.api.bcr_rev = true
  managers.api.reverse_buttons = function() return Global.bcr_pc and Global.bcr_pc.swapconfirm == "on" end
end
if MenuCallbackHandler then
  MenuCallbackHandler.vis_cb_retro_not_unlocked = function() return true end
  MenuCallbackHandler.vis_cb_retro_unlocked = function() return false end
end
if P and not rawget(P, "bcr_music_hooks") then
  P.bcr_music_hooks = true
  local o_markers = P._set_marker_positions
  P._set_marker_positions = function(self, ...)
    local r = o_markers(self, ...)
    if self.name == "bcr_music" and self._left_marker and self._right_marker then
      local ox = self.item_panel and self.item_panel:x() or 0
      self._left_marker:set_width(90)
      self._left_marker:set_x(640 - 310 - 90 - ox)
      self._right_marker:set_width(90)
      self._right_marker:set_x(640 + 310 - ox)
    end
    return r
  end
  local o_hint = P._show_btn_hint
  P._show_btn_hint = function(self, cfg, ...)
    if self.name == "bcr_music" then
      local sw = Global.bcr_pc and Global.bcr_pc.swapconfirm == "on"
      cfg = { { btn = sw and "btn_a" or "btn_b", text = "menu_back" }, { btn = sw and "btn_b" or "btn_a", text = "bcr_hint_start" }, { btn = "btn_x", text = "bcr_hint_stop" } }
    end
    return o_hint(self, cfg, ...)
  end
end
local GAMMA_OPTIONS = {}
for g = 50, 150, 5 do
  STR["bcr_gm_" .. g] = string.format("%.2f", g / 100)
  GAMMA_OPTIONS[#GAMMA_OPTIONS + 1] = { tostring(g), "bcr_gm_" .. g }
end
local function gfx_items(kind)
  local items = {}
  for _, d in ipairs(GFX) do
    if not (kind == "pause" and RESTART_KEYS[d.key]) then items[#items + 1] = toggle_item(d.key, d.text, d.options) end
  end
  items[#items + 1] = { _meta = "item", name = "bcr_gamma_open", text_id = "bcr_pc_gamma", next_node = "bcr_gamma" }
  return items
end
)LUA"
R"LUA(
local MUSIC_EVENTS = {}
do
  local ev = managers.music and managers.music.music_events and managers.music:music_events("")
  if type(ev) == "table" then
    for _, e in pairs(ev) do
      local s = tostring(e)
      if s:find("^[Pp]lay_") then MUSIC_EVENTS[#MUSIC_EVENTS + 1] = s end
    end
  end
  table.sort(MUSIC_EVENTS)
end
local function music_items()
  local track = { _meta = "item", name = "bcr_track", text_id = "bcr_music_track", type = "CoreMenuItemToggle.ItemToggle" }
  for _, e in ipairs(MUSIC_EVENTS) do
    STR["bcr_mt_" .. e] = e
    track[#track + 1] = { _meta = "option", value = e, text_id = "bcr_mt_" .. e }
  end
  return { track }
end
local function extend_settings(menu, kind)
  local sn = menu.data._nodes.settings
  if not sn or sn.bcr_ext then return end
  sn.bcr_ext = true
  local data = { _meta = "node", name = "bcr_tmp" }
  data[#data + 1] = { _meta = "item", name = "bcr_graphics_open", text_id = "bcr_pc_settings", next_node = "graphics_settings" }
  for _, d in ipairs(SETS) do
    if not (kind == "pause" and d.main_only) then data[#data + 1] = toggle_item(d.key, d.text, d.options) end
  end
  if kind ~= "pause" then data[#data + 1] = { _meta = "item", name = "bcr_music_open", text_id = "bcr_music_title", next_node = "bcr_music" } end
  data[#data + 1] = { _meta = "default_item", name = "bcr_graphics_open" }
  local tmp = MenuNode:new(data)
  wire_items(tmp)
  for _, it in ipairs(tmp._items) do
    local nm = it._parameters.name
    if nm == "bcr_graphics_open" then
      table.insert(sn._items, 1, it)
    elseif nm == "bcr_music_open" then
      local at = #sn._items + 1
      for i, x in ipairs(sn._items) do if x._parameters.name == "music_volume" then at = i + 1 end end
      table.insert(sn._items, at, it)
    elseif sn.add_item then
      sn:add_item(it)
    else
      sn._items[#sn._items + 1] = it
    end
  end
  if sn.set_default_item_name then sn:set_default_item_name("bcr_graphics_open") end
  local mod = sn._parameters.modifier
  if type(mod) ~= "table" then mod = {} sn._parameters.modifier = mod end
  mod[#mod + 1] = refresh
  if menu.callback_handler then sn:set_callback_handler(menu.callback_handler) end
end
local function control_items()
  local items = {}
  local options = {}
  for _, s in ipairs(SRC) do options[#options + 1] = { s, "bcr_src_" .. s } end
  for _, a in ipairs(ACTIONS) do items[#items + 1] = toggle_item(a.key, a.text, options) end
  local function fixed(name, text, src)
    return { _meta = "item", name = "bcr_fixed_" .. name, text_id = text, type = "CoreMenuItemToggle.ItemToggle", { _meta = "option", value = src, text_id = "bcr_src_" .. src } }
  end
  items[#items + 1] = fixed("inventory", "bcr_act_inventory", "back")
  items[#items + 1] = fixed("pause", "bcr_act_pause", "start")
  return items
end
local function patch(menu, node_name, item_name, next_node, text_id)
  local node = menu and menu.data and menu.data._nodes and menu.data._nodes[node_name]
  if not node then return end
  for _, item in ipairs(node._items) do
    local p = item._parameters
    if p.name == item_name then
      p.next_node = next_node
      p.callback = {}
      p.callback_name = {}
      if text_id then p.text_id = text_id end
    end
  end
end
for _, which in ipairs({ { "main", main }, { "pause", pause } }) do
  local kind, menu = which[1], which[2]
  if menu and menu.data and menu.data._nodes then
    menu.data._nodes.graphics_settings = build(kind, menu, "graphics_settings", "bcr_pc_settings", gfx_items(kind), "bcr_display", false)
    menu.data._nodes.bcr_gamma = build(kind, menu, "bcr_gamma", "bcr_pc_gamma", { toggle_item("gamma", "bcr_pc_gamma", GAMMA_OPTIONS) }, "bcr_gamma", false, true, true)
    menu.data._nodes.bcr_music = build(kind, menu, "bcr_music", "bcr_music_title", music_items(), "bcr_track", false, true, true)
    extend_settings(menu, kind)
    menu.data._nodes.bcr_controls = build(kind, menu, "bcr_controls", "bcr_controls", control_items(), "bcr_b_jump", true, true)
    local mn = menu.data._nodes[kind]
    if mn and mn._items then
      for i = #mn._items, 1, -1 do
        local nm = mn._items[i]._parameters and mn._items[i]._parameters.name
        if nm and (tostring(nm):find("^leaderboards") or tostring(nm):find("^achievements")) then table.remove(mn._items, i) end
      end
    end
    patch(menu, "help_options", "controls", "bcr_controls", nil)
  end
end
local dlg = reg.menu_dialog
local function make_dialog(name, cap_id, text_id, yes_fn)
  tweak_data.menu.dialog[name] = { caption_id = cap_id, dialog_type = "popup", fade_in = true, fade_in_delay = 0.3, fade_in_speed = 1.5, text_id = text_id }
  local data = { _meta = "node", name = name, config = name, back_item = "bcr_no" }
  data[1] = { _meta = "item", name = "bcr_yes", text_id = "bcr_yes" }
  data[2] = { _meta = "item", name = "bcr_no", text_id = "bcr_no" }
  data[3] = { _meta = "default_item", name = "bcr_no" }
  local dn = MenuNode:new(data)
  local function close_dialog()
    managers.menu:close_dialog_menu()
  end
  for _, it in ipairs(dn._items) do
    if it._parameters.name == "bcr_yes" then
      it._parameters.callback = { function()
        local om = managers.menu._open_menus
        local under = om and om[#om - 1]
        close_dialog()
        yes_fn(under)
      end }
    else
      it._parameters.callback = { close_dialog }
    end
  end
  dn._parameters.back_callback = { close_dialog }
  if dlg.callback_handler then dn:set_callback_handler(dlg.callback_handler) end
  dlg.data._nodes[name] = dn
end
if dlg and dlg.data and dlg.data._nodes and tweak_data and tweak_data.menu and tweak_data.menu.dialog then
  make_dialog("bcr_confirm_exit", "bcr_exit_cap", "bcr_exit_q", function(under)
    Global.bcr_reset_defaults()
    if under and under.logic and under.logic.navigate_back then under.logic:navigate_back(true) end
  end)
  make_dialog("bcr_restart_prompt", "bcr_restart_cap", "bcr_restart_q", function() push("restart", "1") end)
end
return "installed"
)LUA";

// Runs every few frames once the menus are installed: hands over queued changes, tells the menu what the display mode really is (F11 changes it too),
// puts the button bindings into the game's input setup (it is rebuilt for each level), and asks for a reinstall if the game rebuilt its menu data.
const char* kPollLua = R"LUA(
local r = managers and managers.menu and managers.menu._registered_menus
local m = r and r.menu_main
if not (m and m.data and m.data._nodes) then return nil end
if not (m.data._nodes.graphics_settings and m.data._nodes.bcr_controls) then return "reinstall" end
local dg = r.menu_dialog
if dg and dg.data and dg.data._nodes and not (dg.data._nodes.bcr_confirm_exit and dg.data._nodes.bcr_restart_prompt) then return "reinstall" end
local om = managers.menu._open_menus
local top = om and om[#om]
local A = "@ASSIGN@"
if A == "!mplay" or A == "!mstop" then
  local sn = top and top.logic and top.logic:selected_node()
  local tk = sn and sn._items and sn._items[1]
  if A == "!mplay" and tk then
    managers.music:stop()
    managers.music:post_event(tk:value())
  else
    managers.music:stop()
  end
elseif A == "!exit" then
  managers.menu:open_dialog_menu("bcr_confirm_exit")
elseif A ~= "" and Global.bcr_assign then
  local k, s = A:match("^(.-):(.*)$")
  if k then Global.bcr_assign(k, s) end
end
local pc = Global.bcr_pc
if pc and managers.player and (pc.god == "on" or Global.bcr_god_on) then
  local on = pc.god == "on"
  if Player and not Player.bcr_god_die then
    local die, rag = Player.die, Player.ragdoll
    Player.bcr_god_die = true
    Player.die = function(self, killer, is_water)
      if self._unit:bcr_damage()._god_mode then
        die(self, killer, is_water)
        if self._controller and self._controller.enable then self._controller:enable() end
        return
      end
      return die(self, killer, is_water)
    end
    Player.ragdoll = function(self, ...)
      if self._unit:bcr_damage()._god_mode then return end
      return rag(self, ...)
    end
  end
  Global.unlimited_lives = on or nil
  if on and managers.game and managers.game._game_data and managers.game._game_data.player_lives then
    local l = managers.game._game_data.player_lives
    for k, v in pairs(l) do if v < 3 then l[k] = 3 end end
  end
  for _, p in pairs(managers.player:get_players()) do p:bcr_damage()._god_mode = on end
  Global.bcr_god_on = on
end
local gm = managers.game
if pc and gm and gm.set_jump_allowed and (pc.nojump == "on" or Global.bcr_nojump_set) then
  gm:set_jump_allowed(pc.nojump ~= "on")
  Global.bcr_nojump_set = pc.nojump == "on"
end
local MG = rawget(_G, "Map3dNodeGui")
if MG and not rawget(MG, "bcr_hint") then
  local orig_hint = MG._show_btn_hint
  MG.bcr_hint = true
  MG._show_btn_hint = function(self, cfg, ...)
    if type(cfg) == "table" then
      local keep = {}
      for _, e in ipairs(cfg) do if e.text ~= "menu_update_times" then keep[#keep + 1] = e end end
      cfg = keep
    end
    return orig_hint(self, cfg, ...)
  end
end
local MD = rawget(_G, "Map3dData")
if MD and not rawget(MD, "bcr_rows") then
  local orig_init = MD.init
  MD.bcr_rows = true
  MD.init = function(self, ...)
    local res = orig_init(self, ...)
    local d = self._data
    if d then
      for i = #d, 1, -1 do
        local tid = d[i].text_id
        if tid == "map_friend_time" or tid == "map_world_time" or tid == "map_coop" then table.remove(d, i) end
      end
    end
    return res
  end
end
local GR = rawget(_G, "Grenade")
if GR and not rawget(GR, "bcr_td") then
  GR.bcr_td = true
  local orig_un = GR.update_normal
  GR.bcr_orig = orig_un
  GR.update_normal = function(self, time, rel)
    if self._throw_down and self._init_z and managers.level then
      if self.bcr_lob == nil then self.bcr_lob = self._vel.z > 0 end
      if self.bcr_lob and self._vel.z < 0 and self._pos.z < self._init_z - 100 then self._throw_down = false end
      local pos = self._pos
      local ta = managers.level:tilearray()
      local inside = ta:get_tile(pos + Vector3(-10, 0, 0)) or ta:get_tile(pos + Vector3(10, 0, 0)) or ta:get_tile(pos + Vector3(0, 0, -10)) or ta:get_tile(pos + Vector3(0, 0, 10))
      if inside and not self.bcr_in_tile and pos.z < self._init_z - 100 then self._throw_down = false end
      self.bcr_in_tile = inside ~= nil
    end
    return orig_un(self, time, rel)
  end
end
local MMGR = managers.music
local MMT = MMGR and getmetatable(MMGR) and getmetatable(MMGR).__index
if type(MMT) == "table" and not rawget(MMT, "bcr_mlog") then
  MMT.bcr_mlog = true
  Global.bcr_mlog_buf = Global.bcr_mlog_buf or {}
  local LV = {
    level_01_harbor = "1|Puerto Calao", level_02_seaside_base = "2|Fort Arturo", level_03_jungle = "3|The Mighty Jungle", level_04_cave = "4|Agramonte Mines",
    level_05_mountain = "5|Monte Blanco", level_06_scrambler_dish = "6|Scrambler Dish", level_06_boss_megacopter_mk1 = "7|Audience Granted", level_07_ruins = "8|Yultec Ruins",
    level_08_inca = "9|Ancient Chambers", level_09_ceremonial_grounds = "10|Ceremonial Grounds", level_09_boss_gorilla_mk1 = "11|Something Big", level_10_bridge_vertical = "12|Bridge Ascent",
    level_11_bridge_horizontal = "13|Puente del Sabio", level_12_cannon_facilites = "14|Air Defense Facilities", level_12_boss_aa_cannon = "15|AA Platform", level_13_descent = "16|Descent",
    level_14_robot_factory = "17|Robot Factory", level_14_boss_gorilla_mk2 = "18|Something Huge", level_15_deadwood = "19|Deadmarsh", level_16_prison = "20|Prison Break",
    level_17_bionic_labs = "21|Bionic Labs", level_18_boss_bionic_brubaker = "22|Confrontation", level_18_bunker = "23|Reactor", level_19_military_hq = "24|General Headquarters",
    level_20_race = "25|Countdown", level_20_boss_megacopter_mk2 = "26|Reprisal", level_21_missiles = "27|Point of No Return", level_22_skyscraper = "28|Impact",
  }
  local function where()
    local cl = managers.level and managers.level._current_level
    if not cl then return "-|(menus)|-" end
    local short = tostring(cl):gsub("^levels/", "")
    return (LV[short] or "?|" .. short) .. "|" .. short
  end
  local o_post, o_stop = MMT.post_event, MMT.stop
  MMT.post_event = function(self, ev, ...)
    local b = Global.bcr_mlog_buf
    if #b < 60 then b[#b + 1] = "PLAY|" .. where() .. "|" .. tostring(ev) end
    return o_post(self, ev, ...)
  end
  MMT.stop = function(self, ...)
    local b = Global.bcr_mlog_buf
    if #b < 60 then b[#b + 1] = "STOP|" .. where() .. "|" end
    return o_stop(self, ...)
  end
end
local mm = r.menu_map
local sn = mm and mm.data and mm.data._nodes and mm.data._nodes.start_game_node
if sn and not sn.bcr_trim then
  sn.bcr_trim = true
  for i = #sn._items, 1, -1 do
    local nm = sn._items[i]._parameters.name
    if nm == "coop" or nm == "difficulty_with_retro" then table.remove(sn._items, i) end
  end
end
if Global.bcr_apply_binds then Global.bcr_apply_binds() end
local o = Global.bcr_pc_out
Global.bcr_pc_out = nil
if Global.bcr_mlog_buf and #Global.bcr_mlog_buf > 0 then
  for _, e in ipairs(Global.bcr_mlog_buf) do o = (o and (o .. ";") or "") .. "mlog=" .. e end
  Global.bcr_mlog_buf = {}
end
if Global.bcr_pc and not o then Global.bcr_pc.display = "@DISPLAY@" end
local g = top and top.renderer and top.renderer.active_node_gui and top.renderer:active_node_gui()
if g and g._states then
  local sw = pc and pc.swapconfirm == "on"
  local want_sel, want_back = sw and "btn_b" or "btn_a", sw and "btn_a" or "btn_b"
  local changed = false
  for _, st in pairs(g._states) do
    local cfg = type(st) == "table" and st.btn_hint_config or nil
    if cfg then
      for _, e in ipairs(cfg) do
        if e.text == "menu_select" and e.btn ~= want_sel then e.btn = want_sel changed = true
        elseif e.text == "menu_back" and e.btn ~= want_back then e.btn = want_back changed = true
        elseif e.text == "bcr_hint_start" and e.btn ~= want_sel then e.btn = want_sel changed = true end
      end
    end
  end
  local cur_st = g._current_state and g._states[g._current_state]
  if changed and type(cur_st) == "table" and g._show_btn_hint then
    g:_show_btn_hint(cur_st.btn_hint_config)
  end
end
local cap = ""
local n = top and top.logic and top.logic:selected_node()
local nname = n and n:parameters().name
if nname == "bcr_controls" then
  local it = top.logic:selected_item()
  local p = it and it:parameters()
  if p and p.bcr_key and p.bcr_key:sub(1, 2) == "b_" then cap = "assign:" .. p.bcr_key
  elseif p and p.name == "bcr_back" then cap = "exit"
  elseif p and p.name == "bcr_reset" then cap = "reset"
  else cap = "lock" end
elseif nname == "bcr_gamma" then
  cap = "gamma"
elseif nname == "bcr_music" then
  cap = "music"
elseif nname == "start_game_node" then
  cap = "noy"
end
local ui = (om and #om > 0) and "1" or "0"
if ui ~= Global.bcr_ui then
  Global.bcr_ui = ui
  o = (o and (o .. ";") or "") .. "ui=" .. ui
end
if pc and nname == "graphics_settings" and Global.bcr_lastnode ~= "graphics_settings" and Global.bcr_lastnode ~= "bcr_gamma" then
  Global.bcr_gsnap = { scale = pc.scale, outres = pc.outres, vsync = pc.vsync }
end
if pc and top and top.name == "menu_main" and Global.bcr_lastnode == "graphics_settings" and nname and nname ~= "graphics_settings" and nname ~= "bcr_gamma" and Global.bcr_gsnap then
  local s = Global.bcr_gsnap
  Global.bcr_gsnap = nil
  if s.scale ~= pc.scale or s.outres ~= pc.outres or s.vsync ~= pc.vsync then Global.bcr_prompt_wait = 6 end
end
if Global.bcr_prompt_wait then
  Global.bcr_prompt_wait = Global.bcr_prompt_wait - 1
  if Global.bcr_prompt_wait <= 0 then
    Global.bcr_prompt_wait = nil
    managers.menu:open_dialog_menu("bcr_restart_prompt")
  end
end
if Global.bcr_lastnode == "bcr_music" and nname ~= "bcr_music" and managers.music then
  managers.music:stop()
  if top and top.name == "menu_main" then managers.music:post_event(top.music_event or "Play_menu_music") end
end
if nname then Global.bcr_lastnode = nname end
local fp = "@FPS@"
if pc and pc.fps == "on" and Overlay then
  local ft = Global.bcr_fps_txt
  if not (ft and alive(ft)) then
    local ws = Overlay:gui():create_screen_workspace()
    Global.bcr_fps_ws = ws
    ft = ws:panel():text({ text = fp, font = "fonts/merged_bcfont_44", font_size = 28, color = Color(1, 1, 1, 1), layer = 10000, x = 16, y = 10 })
    Global.bcr_fps_txt = ft
    Global.bcr_fps_last = nil
  end
  if Global.bcr_fps_last ~= fp then
    Global.bcr_fps_last = fp
    ft:set_text(fp)
    local _, _, tw, th = ft:text_rect()
    ft:set_size(tw + 8, th + 4)
  end
  ft:set_visible(true)
elseif Global.bcr_fps_txt and alive(Global.bcr_fps_txt) then
  Global.bcr_fps_txt:set_visible(false)
end
local spr = ""
if nname == "bcr_controls" and pc then
  local names = { "jump", "arm", "fire", "upgrade", "next", "prev", "cycleact", "cyclepas", "biovision" }
  local parts = {}
  for i, nm in ipairs(names) do
    parts[#parts + 1] = string.format("%d,%d,40,%s", 860, 114 + 38 * (i - 1), pc["k_" .. nm] or "")
  end
  parts[#parts + 1] = string.format("%d,%d,40,%s", 860, 114 + 38 * 9, "key_tab")
  parts[#parts + 1] = string.format("%d,%d,40,%s", 860, 114 + 38 * 10, "key_escape")
  spr = table.concat(parts, "/")
end
if spr ~= Global.bcr_spr then
  Global.bcr_spr = spr
  o = (o and (o .. ";") or "") .. "spr=" .. spr
end
if cap ~= Global.bcr_cap then
  Global.bcr_cap = cap
  o = (o and (o .. ";") or "") .. "cap=" .. cap
end
return o)LUA";

std::string replace_all(std::string s, const std::string& a, const std::string& b) {
    for (size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += b.size()) s.replace(p, a.size(), b);
    return s;
}

std::string lua_text(const char* embedded) {
    // BCR2_PCMENU_LUA=<file> or BCR2_PCPOLL_LUA=<file>: use that file instead of the built-in text, to try changes without rebuilding.
    const char* env = getenv(embedded == kInstallLua ? "BCR2_PCMENU_LUA" : "BCR2_PCPOLL_LUA");
    if (env) if (FILE* f = fopen(env, "rb")) {
        std::string s; char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        fclose(f);
        return s;
    }
    return embedded;
}

}  // namespace

void settings_apply_to_env() {
    std::lock_guard<std::mutex> lk(lock_());
    load_ini();
    for (const Def& d : kDefs) {
        const std::string& v = values_()[d.key];
        if (!d.env || getenv(d.env)) continue;                         // no switch for this one, or an explicit switch beats the file
        if (!strcmp(d.key, "vsync")) { if (v == "off") _putenv_s(d.env, "1"); }
        else if (!strcmp(d.key, "scale") || !strcmp(d.key, "outres")) { if (v != "auto") _putenv_s(d.env, v.c_str()); }
        else _putenv_s(d.env, v.c_str());
    }
    gpu_set_gamma(atoi(values_()["gamma"].c_str()));
    gpu_set_texfilter(filter_from_name(values_()["filter"]));
    gpu_set_aa(aa_from_name(values_()["aa"]));
    upgrades_set_source(0, values_()["b_cycleact"].c_str());
    upgrades_set_source(1, values_()["b_cyclepas"].c_str());
    fprintf(stderr, "[settings] %s: display=%s scale=%s vsync=%s buttons=%s\n", ini_path().c_str(), values_()["display"].c_str(), values_()["scale"].c_str(), values_()["vsync"].c_str(), values_()["buttons"].c_str());
}

int settings_button_set() { std::lock_guard<std::mutex> lk(lock_()); return button_set_from_name(values_()["buttons"]); }

std::string settings_install_lua() {
    std::lock_guard<std::mutex> lk(lock_());
    std::string init = "{ ";
    for (const Def& d : kDefs) {
        std::string v = !strcmp(d.key, "display") ? display_name(gpu_display_mode()) : values_()[d.key];
        init += std::string(d.key) + " = \"" + v + "\", ";
    }
    init += "}";
    return replace_all(lua_text(kInstallLua), "@INIT@", init);
}

std::string settings_poll_lua() {
    std::string assign;
    {
        std::lock_guard<std::mutex> lk(lock_());
        assign = cap_pending_();
        cap_pending_().clear();
    }
    bool fpsOn; { std::lock_guard<std::mutex> lk(lock_()); fpsOn = values_()["fps"] == "on"; }
    return replace_all(replace_all(replace_all(lua_text(kPollLua), "@DISPLAY@", display_name(gpu_display_mode())), "@ASSIGN@", assign), "@FPS@", fpsOn ? gpu_fps_text() : std::string());
}

bool settings_capture_active() { std::lock_guard<std::mutex> lk(lock_()); return !cap_action_().empty(); }

// cap_action_ is what the menu poll reported for the selected row: "assign:<action>" (press a button to give it to that action), "lock" (a fixed row: no buttons),
// "reset" and "exit" (only the confirm button works; on "exit" the cancel button asks to leave without saving), or "noy" (the level start screen: Y does nothing).
// Keys and mouse buttons into the pad state: menus get the arrows (and WASD), Enter and Escape; the game gets WASD and the arrows as the left stick, Escape as Start, Tab as Back,
// and each action's key presses that action's pad input.
static void kbm_inject_locked(uint8_t* st) {
    if (!kbm_window_active()) return;
    uint16_t raw; memcpy(&raw, st + 4, 2);
    uint16_t w = (uint16_t)((raw >> 8) | (raw << 8));
    int lx = (int16_t)((st[8] << 8) | st[9]), ly = (int16_t)((st[10] << 8) | st[11]);
    const bool ui = ui_open_();
    const std::string& cp = cap_action_();
    const std::string mode = cp.substr(0, cp.find(':'));
    const bool swap = values_()["swapconfirm"] == "on";
    const uint16_t confirmBit = swap ? 0x2000 : 0x1000, cancelBit = swap ? 0x1000 : 0x2000;
    bool up = kbm_reserved_down(VK_UP), down = kbm_reserved_down(VK_DOWN), left = kbm_reserved_down(VK_LEFT), right = kbm_reserved_down(VK_RIGHT);
    const bool wu = kbm_reserved_down('W'), wd = kbm_reserved_down('S'), wl = kbm_reserved_down('A'), wr = kbm_reserved_down('D');
    if (ui) {
        if (mode != "assign") { up |= wu; down |= wd; left |= wl; right |= wr; }
        if (up) w |= 0x0001; if (down) w |= 0x0002; if (left) w |= 0x0004; if (right) w |= 0x0008;
        if (kbm_reserved_down(VK_RETURN)) w |= confirmBit;
        if (kbm_reserved_down(VK_ESCAPE) || kbm_reserved_down(VK_BACK)) w |= cancelBit;
    } else {
        up |= wu; down |= wd; left |= wl; right |= wr;
        if (left) lx = -32767; if (right) lx = 32767; if (up) ly = 32767; if (down) ly = -32767;
        if (kbm_reserved_down(VK_ESCAPE)) w |= 0x0010;
        if (kbm_reserved_down(VK_TAB)) w |= 0x0020;
        static const char* const acts[9] = { "jump", "arm", "fire", "upgrade", "next", "prev", "cycleact", "biovision", "cyclepas" };
        for (const char* a : acts) {
            if (!kbm_down(values_()[std::string("k_") + a])) continue;
            const std::string& s = values_()[std::string("b_") + a];
            if (s == "a") w |= 0x1000; else if (s == "b") w |= 0x2000; else if (s == "x") w |= 0x4000; else if (s == "y") w |= 0x8000;
            else if (s == "lb") w |= 0x0100; else if (s == "rb") w |= 0x0200; else if (s == "l3") w |= 0x0040; else if (s == "r3") w |= 0x0080;
            else if (s == "lt") st[6] = 255; else if (s == "rt") st[7] = 255;
        }
    }
    st[4] = (uint8_t)(w >> 8); st[5] = (uint8_t)w;
    st[8] = (uint8_t)((lx >> 8) & 0xFF); st[9] = (uint8_t)(lx & 0xFF); st[10] = (uint8_t)((ly >> 8) & 0xFF); st[11] = (uint8_t)(ly & 0xFF);
}

static void capture_locked(uint8_t* st) {
    if (cap_action_().empty()) { cap_fresh_() = true; return; }
    const std::string mode = cap_action_().substr(0, cap_action_().find(':'));
    uint16_t raw; memcpy(&raw, st + 4, 2);
    uint16_t w = (uint16_t)((raw >> 8) | (raw << 8));
    if (mode == "gamma") return;                                         // the gamma screen: every button works as normal
    if (mode == "music") {                                               // the music test: X starts the track, Y stops it, and neither reaches the menu
        const uint32_t startBit = values_()["swapconfirm"] == "on" ? 0x2000 : 0x1000;      // the confirm button starts the track, X stops it
        const uint32_t both = startBit | 0x4000;
        uint32_t cur = (uint32_t)(w & both);
        if (cap_fresh_()) { cap_fresh_() = false; cap_prev_() = cur; }
        const uint32_t edge = cur & ~cap_prev_();
        if (edge && cap_pending_().empty()) cap_pending_() = (edge & 0x4000) ? "!mstop" : "!mplay";
        cap_prev_() = cur;
        w &= (uint16_t)~both; st[4] = (uint8_t)(w >> 8); st[5] = (uint8_t)w;
        return;
    }
    if (mode == "noy") { w &= (uint16_t)~0x8000; st[4] = (uint8_t)(w >> 8); st[5] = (uint8_t)w; return; }
    struct B { uint32_t bit; const char* name; };
    static const B kBtn[] = { { 0x1000, "a" }, { 0x2000, "b" }, { 0x4000, "x" }, { 0x8000, "y" }, { 0x0100, "lb" }, { 0x0200, "rb" }, { 0x0040, "l3" }, { 0x0080, "r3" } };
    uint32_t cur = 0;
    for (const B& b : kBtn) if (w & b.bit) cur |= b.bit;
    // triggers count as pressed from 140, released below 60 (the same as the upgrade cycle code)
    const uint32_t kLT = 1u << 16, kRT = 1u << 17;
    uint32_t prev = cap_prev_();
    if (st[6] >= 140 || ((prev & kLT) && st[6] >= 60)) cur |= kLT;
    if (st[7] >= 140 || ((prev & kRT) && st[7] >= 60)) cur |= kRT;
    if (cap_fresh_()) { cap_fresh_() = false; prev = cur; }              // whatever opened the screen is still held: not a choice
    uint32_t edge = cur & ~prev;
    const bool swap = values_()["swapconfirm"] == "on";
    const uint32_t confirmBit = swap ? 0x2000 : 0x1000, cancelBit = swap ? 0x1000 : 0x2000;
    if (edge && cap_pending_().empty()) {
        if (mode == "assign") {
            const char* name = nullptr;
            for (const B& b : kBtn) if (edge & b.bit) { name = b.name; break; }
            if (!name) name = (edge & kLT) ? "lt" : "rt";
            cap_pending_() = cap_action_().substr(cap_action_().find(':') + 1) + ":" + name;
        } else if (mode == "exit" && (edge & cancelBit)) {
            cap_pending_() = "!exit"; cap_block_() = true; fprintf(stderr, "[settings] cancel on Exit: asking\n");
        }
    }
    cap_prev_() = cur;
    uint16_t hide = 0x0010 | 0x0020;                                     // Start and Back never leave this screen either
    for (const B& b : kBtn) hide |= (uint16_t)b.bit;
    if (mode == "reset" || mode == "exit") hide &= (uint16_t)~confirmBit;
    w &= (uint16_t)~hide;
    st[4] = (uint8_t)(w >> 8); st[5] = (uint8_t)w;
    st[6] = 0; st[7] = 0;
}

// With "Swap Confirm & Cancel" on, the bottom and right face buttons trade places while a menu is open (the Lua poll says so). Gameplay keeps the layout set on the Controls screen.
void settings_capture_filter(uint8_t* st) {
    std::lock_guard<std::mutex> lk(lock_());
    kbm_inject_locked(st);
    {   // on the Controls screen a key or mouse button pressed on an action row is given to that action
        const std::string& cp = cap_action_();
        if (cp.compare(0, 7, "assign:") == 0) {
            const std::string kn = kbm_new_press();
            if (!kn.empty() && cap_pending_().empty()) cap_pending_() = cp.substr(7) + ":" + kn;
        }
    }
    uint16_t rawIn; memcpy(&rawIn, st + 4, 2);
    const uint16_t physIn = (uint16_t)((rawIn >> 8) | (rawIn << 8));
    capture_locked(st);
    if (cap_block_()) {                                                  // the cancel press that asked the question must not also answer it
        const uint16_t cancelBit = values_()["swapconfirm"] == "on" ? 0x1000 : 0x2000;
        if (physIn & cancelBit) {
            uint16_t r2; memcpy(&r2, st + 4, 2);
            uint16_t w2 = (uint16_t)((r2 >> 8) | (r2 << 8));
            w2 &= (uint16_t)~cancelBit;
            st[4] = (uint8_t)(w2 >> 8); st[5] = (uint8_t)w2;
        } else cap_block_() = false;
    }
    if (values_()["swapconfirm"] != "on" || !ui_open_()) return;
    uint16_t raw; memcpy(&raw, st + 4, 2);
    uint16_t w = (uint16_t)((raw >> 8) | (raw << 8));
    const bool a = (w & 0x1000) != 0, b = (w & 0x2000) != 0;
    w &= (uint16_t)~0x3000;
    if (a) w |= 0x2000;
    if (b) w |= 0x1000;
    st[4] = (uint8_t)(w >> 8); st[5] = (uint8_t)w;
}

// music_log.txt beside bcr2.ini: one line for each music event the game posts, with the time, the frame, the level number and title, and the event.
uint64_t gpu_frame_number();
static void music_log_append(const std::string& v) {
    static bool started = false;
    std::string path = ini_path(); const size_t p = path.rfind("bcr2.ini"); if (p != std::string::npos) path.replace(p, std::string::npos, "music_log.txt");
    FILE* f = fopen(path.c_str(), "a");
    if (!f) return;
    if (!started) { started = true; SYSTEMTIME s; GetLocalTime(&s); fprintf(f, "\n=== session started %04u-%02u-%02u %02u:%02u:%02u ===\n", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond); }
    // v is "PLAY|<number>|<title>|<level id>|<event>" or "STOP|<number>|<title>|<level id>|"
    std::vector<std::string> a; size_t at = 0;
    for (;;) { size_t e = v.find('|', at); if (e == std::string::npos) { a.push_back(v.substr(at)); break; } a.push_back(v.substr(at, e - at)); at = e + 1; }
    while (a.size() < 5) a.push_back("");
    SYSTEMTIME s; GetLocalTime(&s);
    fprintf(f, "%02u:%02u:%02u  frame %-7llu  level %-2s %-24s (%s)  %s %s\n", s.wHour, s.wMinute, s.wSecond, (unsigned long long)gpu_frame_number(), a[1].c_str(), a[2].c_str(), a[3].c_str(), a[0].c_str(), a[4].c_str());
    fclose(f);
}

// The menu asked for a restart (after the player confirmed): start the same command line again and end this process. BCR2_NORESTART only logs, for tests.
static void restart_host() {
    fprintf(stderr, "[settings] restart requested\n"); fflush(stderr);
    if (getenv("BCR2_NORESTART")) return;
    std::wstring cmd = GetCommandLineW();
    wchar_t wd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, wd);
    STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, wd, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); fprintf(stderr, "[settings] started a new copy, exiting\n"); fflush(stderr); ExitProcess(0); }
    fprintf(stderr, "[settings] could not start a new copy (error %lu); staying open\n", GetLastError());
}

void settings_apply_from_lua(const std::string& changes) {
    std::lock_guard<std::mutex> lk(lock_());
    size_t at = 0;
    bool save = false;
    while (at < changes.size()) {
        size_t end = changes.find(';', at);
        if (end == std::string::npos) end = changes.size();
        std::string item = changes.substr(at, end - at);
        at = end + 1;
        size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string k = item.substr(0, eq), v = item.substr(eq + 1);
        if (k == "cap") { cap_action_() = v; cap_fresh_() = true; if (v.compare(0, 7, "assign:") == 0) kbm_forget_edges(); gpu_set_gamma_preview(v == "gamma"); fprintf(stderr, "[settings] pad capture: %s\n", v.empty() ? "off" : v.c_str()); continue; }
        if (k == "ui") { ui_open_() = v == "1"; continue; }
        if (k == "spr") {                                                   // "x,y,size,name/x,y,size,name/..." or empty: key icons drawn over the Controls screen
            std::vector<GpuSprite> sp; size_t b = 0;
            while (b < v.size()) {
                size_t e = v.find('/', b); if (e == std::string::npos) e = v.size();
                float x = 0, y = 0, s = 0; char nm[64] = {};
                if (sscanf(v.substr(b, e - b).c_str(), "%f,%f,%f,%63s", &x, &y, &s, nm) == 4) if (const char* ic = kbm_icon(nm)) sp.push_back({ ic, x, y, s, s });
                b = e + 1;
            }
            gpu_set_sprites(sp);
            continue;
        }
        if (k == "mlog") { music_log_append(v); continue; }
        if (k == "restart") { if (save) save_ini(); restart_host(); continue; }
        const Def* d = find_def(k);
        if (!d || !allowed(*d, v)) { fprintf(stderr, "[settings] ignoring menu change '%s'\n", item.c_str()); continue; }
        values_()[k] = v;
        save = true;
        if (k == "display") { gpu_set_display_mode(display_from_name(v)); fprintf(stderr, "[settings] display = %s (applied now)\n", v.c_str()); }
        else if (k == "gamma") { gpu_set_gamma(atoi(v.c_str())); }
        else if (k == "filter") { gpu_set_texfilter(filter_from_name(v)); fprintf(stderr, "[settings] texture filtering = %s (applied now)\n", v.c_str()); }
        else if (k == "aa") { gpu_set_aa(aa_from_name(v)); fprintf(stderr, "[settings] antialiasing = %s (applied now)\n", v.c_str()); }
        else if (k == "buttons") { gpu_set_button_set(button_set_from_name(v)); fprintf(stderr, "[settings] buttons = %s (applied now)\n", v.c_str()); }
        else if (k == "b_cycleact") { upgrades_set_source(0, v.c_str()); fprintf(stderr, "[settings] cycle active upgrade = %s (applied now)\n", v.c_str()); }
        else if (k == "b_cyclepas") { upgrades_set_source(1, v.c_str()); fprintf(stderr, "[settings] cycle passive upgrade = %s (applied now)\n", v.c_str()); }
        else if ((k[0] == 'b' && k[1] == '_') || (k[0] == 'k' && k[1] == '_') || k == "nojump" || k == "god" || k == "swapconfirm" || k == "gamma" || k == "fps") fprintf(stderr, "[settings] %s = %s (applied now)\n", k.c_str(), v.c_str());
        else fprintf(stderr, "[settings] %s = %s (takes effect after a restart)\n", k.c_str(), v.c_str());
    }
    if (save) save_ini();
}
