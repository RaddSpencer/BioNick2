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

// The three cosmetics the game treats specially are left out: they are not upgrades. No -- comments, this text can also be handed to the game as one line.
const char* kCycleLua = R"LUA(
local SKIP = { cool_shades = true, dogtags = true, trucker_cap = true }
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
