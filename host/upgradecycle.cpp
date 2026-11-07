// Cycling the equipped arm upgrades from the controller.
//
// The game keeps two slots, "active" and "passive1", and ArmUpgradeSystem:set_component(name) is what its upgrade kiosk calls to change one: it exits the
// old component and enters the new one. Cycling just calls that with the next owned component of the slot. The press itself is read in the controller
// poll (imports.cpp) and carried out here at once, on the game's Lua thread (hooks.cpp), so there is no polling delay.
#include "upgradecycle.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {
std::atomic<int> g_active{0}, g_passive{0};
// Each slot's source as one word: the buttons mask in the low half and, in the high half, 1 for LT or 2 for RT. Defaults: LT cycles active, a right stick click cycles passive.
std::atomic<uint32_t> g_code[2]{ { 1u << 16 }, { 0x0080u } };

// The three cosmetics the game treats specially, and the prestige item (no HUD icon), are left out. No -- comments, this text can also be handed to the game as one line.
const char* kCycleLua = R"LUA(
local SKIP = { cool_shades = true, dogtags = true, trucker_cap = true, ultimate_prestige = true }
local function cycle(slot, n)
  if not (managers and managers.player and managers.menu) then return nil end
  if next(managers.menu._open_menus or {}) then return nil end
  local pl = managers.player:get_player(1)
  if not pl then return nil end
  local sys = pl:arm_components()
  if not (sys and sys._components and sys._slots and sys._slots[slot]) then return nil end
  local names = {}
  for name, c in pairs(sys._components) do
    if c.slot == slot and not SKIP[name] then names[#names + 1] = name end
  end
  if #names < 2 then return nil end
  table.sort(names)
  local cur = sys._slots[slot].component_name
  local idx = 0
  for i, nm in ipairs(names) do if nm == cur then idx = i end end
  idx = (idx + n - 1) % #names + 1
  sys:set_component(names[idx])
  local ss = pl:sound_source()
  if ss then ss:post_event("spencer_weapon_cycle") end
  return slot .. " " .. tostring(cur) .. " -> " .. names[idx]
end
local a, p = @A@, @P@
local r = {}
if a > 0 then r[#r + 1] = cycle("active", a) end
if p > 0 then r[#r + 1] = cycle("passive1", p) end
return table.concat(r, "; ")
)LUA";
}  // namespace

void upgrades_set_source(int slot, const char* name) {
    struct E { const char* n; uint32_t code; };
    static const E table[] = { { "a", 0x1000 }, { "b", 0x2000 }, { "x", 0x4000 }, { "y", 0x8000 }, { "lb", 0x0100 }, { "rb", 0x0200 }, { "lt", 1u << 16 }, { "rt", 2u << 16 },
                               { "back", 0x0020 }, { "start", 0x0010 }, { "l3", 0x0040 }, { "r3", 0x0080 }, { "up", 0x0001 }, { "down", 0x0002 }, { "left", 0x0004 }, { "right", 0x0008 }, { "none", 0 } };
    if (slot < 0 || slot > 1) return;
    for (const E& e : table) if (!strcmp(name, e.n)) { g_code[slot] = e.code; return; }
}

void upgrades_poll_pad(uint16_t buttons, uint8_t lt, uint8_t rt) {
    static bool down[2] = { false, false };
    for (int i = 0; i < 2; i++) {
        const uint32_t c = g_code[i].load();
        const uint16_t mask = (uint16_t)(c & 0xFFFF); const int trig = (int)(c >> 16);
        bool now;
        if (trig == 1) now = down[i] ? lt > 60 : lt > 140;                  // a trigger acts once per pull, with some slack
        else if (trig == 2) now = down[i] ? rt > 60 : rt > 140;
        else now = mask != 0 && (buttons & mask) != 0;
        if (now && !down[i]) upgrades_note_press(i);
        down[i] = now;
    }
}

void upgrades_note_press(int slot) {
    (slot == 0 ? g_active : g_passive)++;
}

bool upgrades_pending() { return g_active.load() > 0 || g_passive.load() > 0; }

std::string upgrades_cycle_lua() {
    std::string s = kCycleLua;
    auto put = [&](const char* tag, int v) { size_t p = s.find(tag); if (p != std::string::npos) s.replace(p, strlen(tag), std::to_string(v)); };
    put("@A@", g_active.exchange(0));
    put("@P@", g_passive.exchange(0));
    return s;
}
