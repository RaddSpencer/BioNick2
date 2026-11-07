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
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

int gpu_display_mode();
void gpu_set_display_mode(int mode);
void gpu_set_button_set(int s);

namespace {

// Function statics: settings_apply_to_env runs from another file's static initialiser, possibly before this file's globals would exist.
std::mutex& lock_() { static std::mutex m; return m; }
std::map<std::string, std::string>& values_() { static std::map<std::string, std::string> v; return v; }

#define BCR_SOURCES "a", "b", "x", "y", "lb", "rb", "lt", "rt", "back", "start", "l3", "r3", "up", "down", "left", "right", "none"
struct Def { const char* key; const char* def; const char* allowed[20]; const char* env; };
const Def kDefs[] = {
    { "display", "window", { "window", "borderless", "fullscreen", nullptr }, "BCR2_DISPLAY" },
    { "scale",   "auto",   { "auto", "1", "2", "3", "4", nullptr },           "BCR2_RESSCALE" },
    { "vrr",     "off",    { "off", "on", nullptr },                          "BCR2_VRR" },
    { "buttons", "classic", { "classic", "xbox", "playstation", "switch", nullptr }, nullptr },
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
};

std::string ini_path() {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string s = exe; size_t p = s.find_last_of('\\');
    return (p == std::string::npos ? std::string(".") : s.substr(0, p)) + "\\..\\bcr2.ini";
}

const Def* find_def(const std::string& key) { for (const Def& d : kDefs) if (key == d.key) return &d; return nullptr; }
bool allowed(const Def& d, const std::string& v) { for (const char* a : d.allowed) if (a && v == a) return true; return false; }

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
        fprintf(f, "# display: window, borderless or fullscreen.  scale: auto, 1, 2, 3 or 4 times 1280x720.  vrr: on for a G-Sync or FreeSync display, else off.  buttons: classic, xbox, playstation or switch.\n");
        fprintf(f, "# b_*: the input that triggers each action: a b x y lb rb lt rt back start l3 r3 up down left right, or none to leave it unbound.\n");
        for (const Def& d : kDefs) fprintf(f, "%s=%s\n", d.key, values_()[d.key].c_str());
        fclose(f);
    }
}

int button_set_from_name(const std::string& v) { return v == "xbox" ? 1 : v == "playstation" ? 2 : v == "switch" ? 3 : 0; }
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
local main, pause = reg.menu_main, reg.menu_pause
if not (main and main.data and main.data._nodes and main.data._nodes.main) then return "wait: menu_main" end
local MenuNode = getmetatable(main.data._nodes.main)
if not (MenuNode and MenuNode.new) then return "wait: node class" end
local STR = {
  bcr_pc_settings = "PC Settings",
  bcr_pc_display = "Display Mode",
  bcr_pc_display_window = "Windowed",
  bcr_pc_display_borderless = "Borderless",
  bcr_pc_display_fullscreen = "Exclusive Fullscreen",
  bcr_pc_scale = "Render Resolution (restart)",
  bcr_pc_scale_auto = "Auto",
  bcr_pc_scale_1 = "720p",
  bcr_pc_scale_2 = "1440p",
  bcr_pc_scale_3 = "2160p",
  bcr_pc_scale_4 = "2880p",
  bcr_pc_vrr = "Variable Refresh (restart)",
  bcr_pc_buttons = "Button Prompts",
  bcr_pc_buttons_classic = "Classic",
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
  bcr_act_upgrade = "Use Arm Upgrade",
  bcr_act_next = "Next Weapon",
  bcr_act_prev = "Previous Weapon",
  bcr_act_cycleact = "Cycle Active Upgrade",
  bcr_act_biovision = "Biovision",
  bcr_act_inventory = "Inventory",
  bcr_act_pause = "Pause Menu",
  bcr_act_cyclepas = "Cycle Passive Upgrade",
}
local LABELS = {
  classic = { a = "A", b = "B", x = "X", y = "Y", lb = "LB", rb = "RB", lt = "LT", rt = "RT", back = "Back", start = "Start", l3 = "L-Stick Click", r3 = "R-Stick Click" },
  xbox = { a = "A", b = "B", x = "X", y = "Y", lb = "LB", rb = "RB", lt = "LT", rt = "RT", back = "View", start = "Menu", l3 = "L-Stick Click", r3 = "R-Stick Click" },
  playstation = { a = "Cross", b = "Circle", x = "Square", y = "Triangle", lb = "L1", rb = "R1", lt = "L2", rt = "R2", back = "Create", start = "Options", l3 = "L3", r3 = "R3" },
  switch = { a = "B", b = "A", x = "Y", y = "X", lb = "L", rb = "R", lt = "ZL", rt = "ZR", back = "Minus", start = "Plus", l3 = "L-Stick Press", r3 = "R-Stick Press" },
}
local GLYPH = { a = 57344, b = 57345, x = 57346, y = 57347, back = 57348, start = 57349, l3 = 57350, r3 = 57351, lb = 57352, rb = 57353, lt = 57354, rt = 57355 }
local function set_labels(set)
  local t = LABELS[set] or LABELS.classic
  for k, v in pairs(t) do STR["bcr_src_" .. k] = GLYPH[k] and utf8.char(GLYPH[k]) or v end
  STR.bcr_src_up = "D-pad Up"
  STR.bcr_src_down = "D-pad Down"
  STR.bcr_src_left = "D-pad Left"
  STR.bcr_src_right = "D-pad Right"
  STR.bcr_src_none = "Unbound"
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
local DEFS = {
  { key = "display", text = "bcr_pc_display", options = { { "window", "bcr_pc_display_window" }, { "borderless", "bcr_pc_display_borderless" }, { "fullscreen", "bcr_pc_display_fullscreen" } } },
  { key = "scale", text = "bcr_pc_scale", options = { { "auto", "bcr_pc_scale_auto" }, { "1", "bcr_pc_scale_1" }, { "2", "bcr_pc_scale_2" }, { "3", "bcr_pc_scale_3" }, { "4", "bcr_pc_scale_4" } } },
  { key = "vrr", text = "bcr_pc_vrr", options = { { "off", "bcr_pc_off" }, { "on", "bcr_pc_on" } } },
  { key = "buttons", text = "bcr_pc_buttons", options = { { "classic", "bcr_pc_buttons_classic" }, { "xbox", "bcr_pc_buttons_xbox" }, { "playstation", "bcr_pc_buttons_playstation" }, { "switch", "bcr_pc_buttons_switch" } } },
}
local SRC = { "a", "b", "x", "y", "lb", "rb", "lt", "rt", "back", "start", "l3", "r3", "up", "down", "left", "right", "none" }
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
      local want = PHYS[src]
      for _, nm in ipairs(a.names) do
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
local function toggle_item(key, text, options)
  local item = { _meta = "item", name = "bcr_" .. key, bcr_key = key, text_id = text, type = "CoreMenuItemToggle.ItemToggle" }
  for _, o in ipairs(options) do item[#item + 1] = { _meta = "option", value = o[1], text_id = o[2] } end
  return item
end
local function build(kind, menu, name, header, items, default_name, with_reset, straight)
  local data = { _meta = "node", name = name }
  local flat = kind == "pause" or straight
  if straight and kind == "main" then data.camera_position = "left_outside" end
  if flat then data.header = header else data.header_id = header end
  for _, it in ipairs(items) do data[#data + 1] = it end
  if with_reset then data[#data + 1] = { _meta = "item", name = "bcr_reset", text_id = "bcr_reset" } end
  data[#data + 1] = { _meta = "default_item", name = default_name }
  data[#data + 1] = { _meta = "node_type", name = flat and "MenuPauseNodeGui" or "Menu3dNodeGui" }
  local node = MenuNode:new(data)
  for _, it in ipairs(node._items) do
    local key = it._parameters.bcr_key
    if key then
      it._parameters.callback = { function(self)
        local v = self:value()
        Global.bcr_pc[key] = v
        if key == "buttons" and Global.bcr_set_labels then Global.bcr_set_labels(v) end
        push(key, v)
        Global.bcr_apply_binds()
      end }
    elseif it._parameters.name == "bcr_reset" then
      it._parameters.callback = { function()
        for _, a in ipairs(ACTIONS) do
          Global.bcr_pc[a.key] = a.def
          push(a.key, a.def)
        end
        refresh(node)
        Global.bcr_apply_binds()
      end }
    end
  end
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
      parameters._stripe_panel_y = 88
    end
    local r = orig(self, node, layer, parameters, ...)
    if np and np.name == "bcr_controls" and self.item_panel then self.item_panel:set_y(self.item_panel:y() - 112) end
    if np and np.name == "bcr_controls" and self.row_items then
      for _, ri in pairs(self.row_items) do
        local gp = ri.gui_panel
        if gp then gp:set_height(gp:height() + 30) end
      end
    end
    return r
  end
end
local function settings_items()
  local items = {}
  for _, d in ipairs(DEFS) do items[#items + 1] = toggle_item(d.key, d.text, d.options) end
  return items
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
    menu.data._nodes.pc_settings = build(kind, menu, "pc_settings", "bcr_pc_settings", settings_items(), "bcr_display", false)
    menu.data._nodes.bcr_controls = build(kind, menu, "bcr_controls", "bcr_controls", control_items(), "bcr_b_jump", true, true)
    patch(menu, kind, "achievements_x360", "pc_settings", "bcr_pc_settings")
    local mn = menu.data._nodes[kind]
    if mn and mn._items then
      for i = #mn._items, 1, -1 do
        local nm = mn._items[i]._parameters and mn._items[i]._parameters.name
        if nm and tostring(nm):find("^leaderboards") then table.remove(mn._items, i) end
      end
    end
    patch(menu, "help_options", "controls", "bcr_controls", nil)
  end
end
return "installed"
)LUA";

// Runs every few frames once the menus are installed: hands over queued changes, tells the menu what the display mode really is (F11 changes it too),
// puts the button bindings into the game's input setup (it is rebuilt for each level), and asks for a reinstall if the game rebuilt its menu data.
const char* kPollLua = R"LUA(
local r = managers and managers.menu and managers.menu._registered_menus
local m = r and r.menu_main
if not (m and m.data and m.data._nodes) then return nil end
if not (m.data._nodes.pc_settings and m.data._nodes.bcr_controls) then return "reinstall" end
if Global.bcr_apply_binds then Global.bcr_apply_binds() end
local o = Global.bcr_pc_out
Global.bcr_pc_out = nil
if Global.bcr_pc and not o then Global.bcr_pc.display = "@DISPLAY@" end
return o
)LUA";

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
        if (!strcmp(d.key, "vrr")) { if (v == "on") _putenv_s(d.env, "1"); }
        else if (!strcmp(d.key, "scale")) { if (v != "auto") _putenv_s(d.env, v.c_str()); }
        else _putenv_s(d.env, v.c_str());
    }
    upgrades_set_source(0, values_()["b_cycleact"].c_str());
    upgrades_set_source(1, values_()["b_cyclepas"].c_str());
    fprintf(stderr, "[settings] %s: display=%s scale=%s vrr=%s buttons=%s\n", ini_path().c_str(), values_()["display"].c_str(), values_()["scale"].c_str(), values_()["vrr"].c_str(), values_()["buttons"].c_str());
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
    return replace_all(lua_text(kPollLua), "@DISPLAY@", display_name(gpu_display_mode()));
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
        const Def* d = find_def(k);
        if (!d || !allowed(*d, v)) { fprintf(stderr, "[settings] ignoring menu change '%s'\n", item.c_str()); continue; }
        values_()[k] = v;
        save = true;
        if (k == "display") { gpu_set_display_mode(display_from_name(v)); fprintf(stderr, "[settings] display = %s (applied now)\n", v.c_str()); }
        else if (k == "buttons") { gpu_set_button_set(button_set_from_name(v)); fprintf(stderr, "[settings] buttons = %s (applied now)\n", v.c_str()); }
        else if (k == "b_cycleact") { upgrades_set_source(0, v.c_str()); fprintf(stderr, "[settings] cycle active upgrade = %s (applied now)\n", v.c_str()); }
        else if (k == "b_cyclepas") { upgrades_set_source(1, v.c_str()); fprintf(stderr, "[settings] cycle passive upgrade = %s (applied now)\n", v.c_str()); }
        else if (k[0] == 'b' && k[1] == '_') fprintf(stderr, "[settings] %s = %s (applied now)\n", k.c_str(), v.c_str());
        else fprintf(stderr, "[settings] %s = %s (takes effect after a restart)\n", k.c_str(), v.c_str());
    }
    if (save) save_ini();
}
