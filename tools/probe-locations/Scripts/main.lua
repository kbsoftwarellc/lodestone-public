--[[ Phase C1 probe — does UPalLocationManager exist, and what does it hold?

Read-only. Logs and exits. Nothing is written to the save, no game state is touched.

Why this exists
---------------
The whole POI plan (Fast Travel, Dungeons, Alphas, Sealed Realms, Towers, Bounties)
rests on one unverified claim: that UPalLocationManager holds every location in
LocationMapCombined, live-queryable, so we need no extracted coordinates at all.

That claim comes from Source/Pal/Public/PalLocationManager.h -- a header from the
inherited PalworldModdingKit dump, NOT from the shipped game. A string scan of
Palworld-Win64-Shipping.exe found *zero* occurrences of "PalLocationManager", which
nobody could reconcile. So the class may be renamed, may be stale, or the scan may
have missed it. Building POI layers on a header that doesn't match the running game
would waste days.

This probe answers it in one launch, before any C++ is written.

The second question it answers is the go/no-go: are UNDISCOVERED locations
registered? If the manager only knows about places you've already found, a live POI
layer shows you nothing the map doesn't already show, and the whole approach is
worthless -- we'd fall back to extracting coordinates from the pak.

There's a live hint that this matters: the mod calibrates on 137 anchors while
kStatues holds 152, and palworld.gg independently lists Fast Travel at exactly 137.
Suspicious. If 137 is "discovered/visible" and 152 is "all", that's the answer.

Install
-------
  cp -r tools/probe-locations <ue4ss>/Mods/ProbeLocations
  echo "ProbeLocations : 1" >> <ue4ss>/Mods/mods.txt     (keep Keybinds last)
Then load a save and press Ctrl+Numpad9. Read UE4SS.log for [Probe] lines.
Remove the mods.txt line when done.
]]

local function log(fmt, ...)
    local ok, s = pcall(string.format, fmt, ...)
    print("[Probe] " .. (ok and s or fmt) .. "\n")
end

-- UE4SS's FindAllOf/StaticFindObject throw on unknown names rather than returning
-- nil, so every lookup here is wrapped. A probe that crashes the game teaches us
-- nothing and costs a relaunch.
local function try(f, ...)
    local ok, r = pcall(f, ...)
    if ok then return r end
    return nil, tostring(r)
end

-- Candidate names. The modding-kit header says UPalLocationManager; the EXE scan
-- says that string isn't there. Both can't be right, so try every plausible spelling
-- rather than guessing one and concluding "absent" from a typo.
local CANDIDATES = {
    "PalLocationManager", "UPalLocationManager",
    "PalLocationSubsystem", "PalWorldLocationManager",
    "PalMapObjectManager", "PalLocationBase",
    "PalLocationInfo", "PalLocationPoint",
}

local function probe_classes()
    log("--- class resolution ---")
    local found = {}
    for _, name in ipairs(CANDIDATES) do
        local insts, err = try(FindAllOf, name)
        if insts and #insts > 0 then
            log("  FOUND  %-26s  %d instance(s)  first=%s", name, #insts, insts[1]:GetFullName())
            found[name] = insts
        elseif insts then
            log("  empty  %-26s  (class known, 0 live instances)", name)
        else
            log("  absent %-26s", name)
        end
    end
    return found
end

-- Walk every UObject once and report anything location-manager-shaped. This is the
-- backstop: if the class was renamed, CANDIDATES misses it but this finds it.
local function scan_objects()
    log("--- UObject scan for location-ish classes ---")
    local seen, n = {}, 0
    local ok, err = pcall(function()
        ForEachUObject(function(obj)
            n = n + 1
            local cls = obj:GetClass()
            if not cls or not cls:IsValid() then return end
            local cn = cls:GetFName():ToString()
            if cn:lower():find("location") or cn:lower():find("marker") then
                seen[cn] = (seen[cn] or 0) + 1
            end
        end)
    end)
    if not ok then log("  scan failed: %s", tostring(err)) return end
    log("  scanned %d objects", n)
    local keys = {}
    for k in pairs(seen) do keys[#keys + 1] = k end
    table.sort(keys)
    if #keys == 0 then log("  no class name contains 'location' or 'marker'") end
    for _, k in ipairs(keys) do log("  %-46s x%d", k, seen[k]) end
end

-- The go/no-go. Count what the manager actually holds, and bucket by type. If the
-- counts land near palworld.gg's (Fast Travel 137, Dungeons 157, Towers 9) then the
-- manager knows about undiscovered places and live POI layers are viable.
local function probe_map(insts)
    log("--- LocationMapCombined / GetLocationMap ---")
    for name, list in pairs(insts) do
        local mgr = list[1]
        log("  on %s:", name)
        for _, prop in ipairs({ "LocationMapCombined", "LocationMap", "Locations" }) do
            local v, err = try(function() return mgr[prop] end)
            if v ~= nil then
                local cnt = try(function() return #v end)
                log("    prop %-22s -> %s (len=%s)", prop, type(v), tostring(cnt))
            end
        end
        for _, fn in ipairs({ "GetLocationMap", "GetLocationMapCombined", "GetCustomMarkers" }) do
            local f = try(function() return mgr[fn] end)
            if f ~= nil then
                log("    func %-22s -> present", fn)
                local r, err = try(function() return mgr[fn](mgr) end)
                if r ~= nil then
                    log("      returned %s", type(r))
                else
                    -- Expected for TMap: UE4SS reflection often cannot marshal a
                    -- TMap return by value. Not a failure of the plan, just of Lua.
                    log("      call not marshalable from Lua (%s)", tostring(err):sub(1, 90))
                end
            end
        end
    end
end

RegisterKeyBind(Key.NUM_NINE, { ModifierKey.CONTROL }, function()
    log("=== Phase C1 probe start ===")
    local found = probe_classes()
    if next(found) == nil then
        log("no candidate class resolved -> falling back to full UObject scan")
    end
    scan_objects()
    if next(found) ~= nil then probe_map(found) end
    log("=== Phase C1 probe end ===")
end)

log("ProbeLocations loaded — load a save, then press Ctrl+Numpad9")
