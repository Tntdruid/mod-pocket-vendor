-- PocketVendor.lua – talks to the mod-pocket-vendor server module
--
-- The server is the source of truth: lists are stored per character in the
-- characters database. This file only sends requests and mirrors the answers.
--
-- Client -> server (addon whisper to yourself, prefix PVEND):
--   SYNC | ADD:<S|K>:<id>[:ALL] | DEL:<id> | TOGGLE:<0|1> | SHOP | BUY:<id>:<lots>
--   (:ALL adds every rank of the item, e.g. Scroll of Spirit I-IV)
-- Server -> client:
--   STATE:<module on>:<auto-sell on>:<toggle allowed>
--   LIST:BEGIN | LIST:S:<id,id,...> | LIST:K:<id,id,...> | LIST:END
--   ADDED:<S|K>:<id,id,...> | REMOVED:<id> | ERR:<text>
--   SHOP:STATE:<0|1> | SHOP:BEGIN | SHOP:ITEM:<A|R|P|F>:<id>:<lot size>:<lot price> | SHOP:END
--   BOUGHT:<id>:<count>:<cost>

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
    shop = {},          -- { { category, id, lotSize, lotPrice }, ... }
    shopEnabled = true,
    shopLoaded = false,
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

local function SendAdd(id, listKey, allRanks)
    Send((listKey == "keep" and "ADD:K:" or "ADD:S:") .. id .. (allRanks and ":ALL" or ""))
end

StaticPopupDialogs["POCKET_VENDOR_ALL_RANKS"] = {
    text = "Add every rank of %s to your %s list, or only the one you picked?",
    button1 = "All ranks",
    button2 = "Only this",
    OnAccept = function(self, data)
        SendAdd(data.id, data.listKey, true)
    end,
    -- Escape also cancels; only the button adds the single item
    OnCancel = function(self, data, reason)
        if reason == "clicked" then
            SendAdd(data.id, data.listKey, false)
        end
    end,
    timeout = 0,
    whileDead = true,
    hideOnEscape = true,
    preferredIndex = 3,
}

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

    -- Ranked items such as "Scroll of Spirit IV": offer to add the whole series
    local name = GetItemInfo(id)
    local baseName = name and name:match("^(.+) [IVXL]+$")
    if baseName then
        local listName = listKey == "keep" and "|cff00ff00Keep|r" or "|cffffd700Sell|r"
        local dialog = StaticPopup_Show("POCKET_VENDOR_ALL_RANKS", "|cffffffff" .. baseName .. "|r", listName)
        if dialog then
            dialog.data = { id = id, listKey = listKey }
            return
        end
    end

    SendAdd(id, listKey, false)
end

function addon:RemoveItem(id)
    if id and self.connected then
        Send("DEL:" .. id)
    end
end

function addon:RequestShop()
    if self.connected then
        Send("SHOP")
    end
end

-- Buys whole lots: one lot is the vendor's stack, e.g. 200 arrows
function addon:Buy(id, lots)
    if id and self.connected then
        Send("BUY:" .. id .. ":" .. (lots or 1))
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

    local addKind, addIds = body:match("^ADDED:([SK]):([%d,]+)$")
    if addKind then
        local target = addKind == "S" and addon.sell or addon.keep
        local added = {}
        for idText in addIds:gmatch("%d+") do
            local id = tonumber(idText)
            -- An item lives on one list only
            addon.sell[id] = nil
            addon.keep[id] = nil
            target[id] = true
            added[#added + 1] = id
        end

        local listName = addKind == "S" and "|cffffd700sell|r" or "|cff00ff00keep|r"
        local name = GetItemInfo(added[1]) or ("item:" .. added[1])
        if #added == 1 then
            Print(("Added %s to your %s list."):format(name, listName))
        else
            Print(("Added %d ranks of %s to your %s list."):format(#added, name:match("^(.+) [IVXL]+$") or name, listName))
        end
        return Refresh()
    end

    local shopState = body:match("^SHOP:STATE:(%d)$")
    if shopState then
        addon.shopEnabled = shopState == "1"
        return
    end

    if body == "SHOP:BEGIN" then
        pending.shop = {}
        return
    end

    local category, shopId, lotSize, lotPrice = body:match("^SHOP:ITEM:(%a):(%d+):(%d+):(%d+)$")
    if category and pending.shop then
        table.insert(pending.shop, {
            category = category, id = tonumber(shopId), lotSize = tonumber(lotSize), lotPrice = tonumber(lotPrice),
        })
        return
    end

    if body == "SHOP:END" then
        addon.shop = pending.shop or {}
        addon.shopLoaded = true
        pending.shop = nil
        return Refresh()
    end

    if body:match("^BOUGHT:") then
        -- The server already told the player in chat; just redraw prices against the new money
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
-- What the shop offers depends on level, skills and known spells
events:RegisterEvent("PLAYER_LEVEL_UP")
events:RegisterEvent("LEARNED_SPELL_IN_TAB")
events:RegisterEvent("SKILL_LINES_CHANGED")
events:RegisterEvent("PLAYER_MONEY")
events:SetScript("OnEvent", function(_, event, prefix, message)
    if event == "CHAT_MSG_ADDON" then
        if prefix == PREFIX then
            OnServerMessage(message)
        end
    elseif event == "PLAYER_ENTERING_WORLD" then
        -- Only the first load screen; later ones keep the lists we have
        events:UnregisterEvent("PLAYER_ENTERING_WORLD")
        addon:Sync()
    elseif event == "PLAYER_MONEY" then
        Refresh()
    else
        addon.shopLoaded = false
        if addon.activeTab == "shop" and addon.UIFrame and addon.UIFrame:IsShown() then
            addon:RequestShop()
        end
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
