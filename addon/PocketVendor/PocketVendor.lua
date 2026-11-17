-- PocketVendor.lua – talks to the mod-pocket-vendor server module
--
-- The server is the source of truth: lists are stored per character in the
-- characters database. This file only sends requests and mirrors the answers.
--
-- Client -> server (addon whisper to yourself, prefix PVEND):
--   SYNC | ADD:S:<id> | ADD:K:<id> | DEL:<id> | TOGGLE:<0|1>
-- Server -> client:
--   STATE:<module on>:<auto-sell on>:<toggle allowed>
--   LIST:BEGIN | LIST:S:<id,id,...> | LIST:K:<id,id,...> | LIST:END
--   ADDED:<S|K>:<id> | REMOVED:<id> | ERR:<text>

local ADDON_NAME = ...
local PREFIX = "PVEND"
local SYNC_TIMEOUT = 5

local addon = {
    sell = {},          -- [itemID] = true
    keep = {},          -- [itemID] = true
    connected = false,  -- the server module answered
    moduleEnabled = true,
    autoSell = true,
    toggleAllowed = true,
    activeTab = "sell",
}
_G[ADDON_NAME] = addon

local pending = {}      -- list being received between LIST:BEGIN and LIST:END

local function Print(msg)
    DEFAULT_CHAT_FRAME:AddMessage("|cff00ccff[Pocket Vendor]|r " .. msg)
end
addon.Print = Print

local function Send(body)
    SendAddonMessage(PREFIX, body, "WHISPER", UnitName("player"))
end

local function Refresh()
    if addon.RefreshUI then addon:RefreshUI() end
end

local function ParseIds(text, target)
    for id in text:gmatch("%d+") do
        target[tonumber(id)] = true
    end
end

---------------------------------------------------------------------
-- PUBLIC API (used by the UI)
---------------------------------------------------------------------
function addon:ToItemId(input)
    if not input then return nil end
    local n = tonumber(input)
    if n then return n end
    return tonumber(tostring(input):match("item:(%d+)"))
end

function addon:Sync()
    Send("SYNC")
    addon.syncSentAt = GetTime()
end

function addon:AddItem(input, listKey)
    local id = self:ToItemId(input)
    if not id then
        Print("Drop an item, shift-click a link or type an item ID.")
        return
    end
    if not self.connected then
        Print("|cffff8040The server module is not responding.|r")
        return
    end
    Send((listKey == "keep" and "ADD:K:" or "ADD:S:") .. id)
end

function addon:RemoveItem(id)
    if id and self.connected then
        Send("DEL:" .. id)
    end
end

function addon:SetAutoSell(enabled)
    Send("TOGGLE:" .. (enabled and "1" or "0"))
end

function addon:GetList(listKey)
    return listKey == "keep" and self.keep or self.sell
end

---------------------------------------------------------------------
-- SERVER MESSAGES
---------------------------------------------------------------------
local function OnServerMessage(body)
    local state = { body:match("^STATE:(%d):(%d):(%d)$") }
    if state[1] then
        addon.connected = true
        addon.moduleEnabled = state[1] == "1"
        addon.autoSell = state[2] == "1"
        addon.toggleAllowed = state[3] == "1"
        return Refresh()
    end

    if body == "LIST:BEGIN" then
        pending = { sell = {}, keep = {} }
        return
    end

    if body == "LIST:END" then
        addon.sell = pending.sell or {}
        addon.keep = pending.keep or {}
        pending = {}
        return Refresh()
    end

    local kind, ids = body:match("^LIST:([SK]):(.+)$")
    if kind and pending.sell then
        ParseIds(ids, kind == "S" and pending.sell or pending.keep)
        return
    end

    local addKind, addId = body:match("^ADDED:([SK]):(%d+)$")
    if addKind then
        addId = tonumber(addId)
        -- An item lives on one list only
        addon.sell[addId] = nil
        addon.keep[addId] = nil
        local target = addKind == "S" and addon.sell or addon.keep
        target[addId] = true

        local name = GetItemInfo(addId) or ("item:" .. addId)
        Print(("Added %s to your %s list."):format(name, addKind == "S" and "|cffffd700sell|r" or "|cff00ff00keep|r"))
        return Refresh()
    end

    local removedId = body:match("^REMOVED:(%d+)$")
    if removedId then
        removedId = tonumber(removedId)
        addon.sell[removedId] = nil
        addon.keep[removedId] = nil
        return Refresh()
    end

    local err = body:match("^ERR:(.+)$")
    if err then
        Print("|cffff8040" .. err .. "|r")
    end
end

---------------------------------------------------------------------
-- EVENTS
---------------------------------------------------------------------
local events = CreateFrame("Frame")
events:RegisterEvent("CHAT_MSG_ADDON")
events:RegisterEvent("PLAYER_ENTERING_WORLD")
events:SetScript("OnEvent", function(_, event, prefix, message)
    if event == "CHAT_MSG_ADDON" then
        if prefix == PREFIX then
            OnServerMessage(message)
        end
    elseif event == "PLAYER_ENTERING_WORLD" then
        -- Only the first load screen; later ones keep the lists we have
        events:UnregisterEvent("PLAYER_ENTERING_WORLD")
        addon:Sync()
    end
end)

-- Report once if the server never answers the first SYNC
events:SetScript("OnUpdate", function(self)
    if addon.connected then
        self:SetScript("OnUpdate", nil)
    elseif addon.syncSentAt and GetTime() - addon.syncSentAt > SYNC_TIMEOUT then
        self:SetScript("OnUpdate", nil)
        Refresh()
    end
end)

---------------------------------------------------------------------
-- SLASH COMMANDS
---------------------------------------------------------------------
SLASH_POCKETVENDOR1 = "/pv"
SLASH_POCKETVENDOR2 = "/pocketvendor"
SlashCmdList["POCKETVENDOR"] = function(msg)
    msg = (msg or ""):lower():match("^%s*(.-)%s*$")
    if msg == "sync" then
        addon:Sync()
        Print("Reloading lists from the server.")
    else
        addon:ToggleUI()
    end
end
