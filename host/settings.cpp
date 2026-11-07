// Settings file and the in-game PC Settings menu.
//
// The menu replaces the Achievements entry of the game's main and pause menus (nothing in this build can show achievements) and opens a node of the
// game's own toggle items. It is built at run time, in Lua, from the same node and item classes the game's menu files use, so the game assets stay as
// they are. The menu only talks to the host through one Lua global: each change appends "key=value" to Global.bcr_pc_out, and the host collects that
// a few times a second (hooks.cpp) and applies it here.
//
// Settings that the renderer reads once at start-up (render scale, variable refresh) are saved and take effect on the next start. The display mode
// is applied at once.
#include "settings.h"
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

struct Def { const char* key; const char* def; const char* allowed[6]; const char* env; };
const Def kDefs[] = {
    { "display", "window", { "window", "borderless", "fullscreen", nullptr }, "BCR2_DISPLAY" },
    { "scale",   "auto",   { "auto", "1", "2", "3", "4", nullptr },           "BCR2_RESSCALE" },
    { "vrr",     "off",    { "off", "on", nullptr },                          "BCR2_VRR" },
    { "buttons", "classic", { "classic", "xbox", "playstation", "switch", nullptr }, nullptr },
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
        fprintf(f, "# Bionic Commando Rearmed 2 (recomp) settings. Written by the PC Settings menu; you can edit it by hand while the game is closed.\n");
        fprintf(f, "# display: window, borderless or fullscreen.  scale: auto, 1, 2, 3 or 4 times 1280x720.  vrr: on for a G-Sync or FreeSync display, else off.  buttons: classic, xbox, playstation or switch.\n");
        for (const Def& d : kDefs) fprintf(f, "%s=%s\n", d.key, values_()[d.key].c_str());
        fclose(f);
    }
}

int button_set_from_name(const std::string& v) { return v == "xbox" ? 1 : v == "playstation" ? 2 : v == "switch" ? 3 : 0; }
int display_from_name(const std::string& v) { return v == "borderless" ? 1 : v == "fullscreen" ? 2 : 0; }
const char* display_name(int m) { return m == 1 ? "borderless" : m == 2 ? "fullscreen" : "window"; }

// The PC Settings menu, in Lua. @INIT@ becomes a table of the current values. No -- comments: this text is also handed to the game as one line while testing.
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
}
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
Global.bcr_pc = Global.bcr_pc or { display = INIT.display, scale = INIT.scale, vrr = INIT.vrr, buttons = INIT.buttons }
local DEFS = {
  { key = "display", text = "bcr_pc_display", options = { { "window", "bcr_pc_display_window" }, { "borderless", "bcr_pc_display_borderless" }, { "fullscreen", "bcr_pc_display_fullscreen" } } },
  { key = "scale", text = "bcr_pc_scale", options = { { "auto", "bcr_pc_scale_auto" }, { "1", "bcr_pc_scale_1" }, { "2", "bcr_pc_scale_2" }, { "3", "bcr_pc_scale_3" }, { "4", "bcr_pc_scale_4" } } },
  { key = "vrr", text = "bcr_pc_vrr", options = { { "off", "bcr_pc_off" }, { "on", "bcr_pc_on" } } },
  { key = "buttons", text = "bcr_pc_buttons", options = { { "classic", "bcr_pc_buttons_classic" }, { "xbox", "bcr_pc_buttons_xbox" }, { "playstation", "bcr_pc_buttons_playstation" }, { "switch", "bcr_pc_buttons_switch" } } },
}
local function refresh(node)
  local cur = Global.bcr_pc
  for _, it in ipairs(node._items) do
    local key = it._parameters.bcr_key
    if key and cur[key] ~= nil then it:set_value(cur[key]) end
  end
  return node
end
local function build(kind, menu)
  local data = { _meta = "node", name = "pc_settings" }
  if kind == "pause" then data.header = "bcr_pc_settings" else data.header_id = "bcr_pc_settings" end
  for _, d in ipairs(DEFS) do
    local item = { _meta = "item", name = "bcr_" .. d.key, bcr_key = d.key, text_id = d.text, type = "CoreMenuItemToggle.ItemToggle" }
    for _, o in ipairs(d.options) do item[#item + 1] = { _meta = "option", value = o[1], text_id = o[2] } end
    data[#data + 1] = item
  end
  data[#data + 1] = { _meta = "default_item", name = "bcr_display" }
  data[#data + 1] = { _meta = "node_type", name = kind == "pause" and "MenuPauseNodeGui" or "Menu3dNodeGui" }
  local node = MenuNode:new(data)
  for _, it in ipairs(node._items) do
    local key = it._parameters.bcr_key
    if key then
      it._parameters.callback = { function(self)
        local v = self:value()
        Global.bcr_pc[key] = v
        Global.bcr_pc_out = (Global.bcr_pc_out and (Global.bcr_pc_out .. ";") or "") .. key .. "=" .. tostring(v)
      end }
    end
  end
  node._parameters.modifier = { refresh }
  if menu.callback_handler then node:set_callback_handler(menu.callback_handler) end
  return node
end
local function patch(menu, node_name)
  local node = menu and menu.data and menu.data._nodes and menu.data._nodes[node_name]
  if not node then return end
  for _, item in ipairs(node._items) do
    local p = item._parameters
    if p.name == "achievements_x360" then
      p.next_node = "pc_settings"
      p.callback = {}
      p.callback_name = {}
      p.text_id = "bcr_pc_settings"
    end
  end
end
main.data._nodes.pc_settings = build("main", main)
patch(main, "main")
if pause and pause.data and pause.data._nodes then
  pause.data._nodes.pc_settings = build("pause", pause)
  patch(pause, "pause")
end
return "installed"
)LUA";

// Runs every few frames once the menu is installed: hands over queued changes, tells the menu what the display mode really is (F11 changes it too),
// and asks for a reinstall if the game rebuilt its menu data.
const char* kPollLua = R"LUA(
local r = managers and managers.menu and managers.menu._registered_menus
local m = r and r.menu_main
if not (m and m.data and m.data._nodes) then return nil end
if not m.data._nodes.pc_settings then return "reinstall" end
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
    fprintf(stderr, "[settings] %s: display=%s scale=%s vrr=%s\n", ini_path().c_str(), values_()["display"].c_str(), values_()["scale"].c_str(), values_()["vrr"].c_str());
}

int settings_button_set() { std::lock_guard<std::mutex> lk(lock_()); return button_set_from_name(values_()["buttons"]); }

std::string settings_install_lua() {
    std::lock_guard<std::mutex> lk(lock_());
    std::string init = "{ display = \"" + std::string(display_name(gpu_display_mode())) + "\", scale = \"" + values_()["scale"] + "\", vrr = \"" + values_()["vrr"] + "\", buttons = \"" + values_()["buttons"] + "\" }";
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
        else fprintf(stderr, "[settings] %s = %s (takes effect after a restart)\n", k.c_str(), v.c_str());
    }
    if (save) save_ini();
}
