-- PocketVendorUI.lua – dark panel with Sell/Keep tabs, drop zone and item list
local ADDON_NAME = ...
local addon = _G[ADDON_NAME]

local ROW_HEIGHT = 22
local VISIBLE_ROWS = 12
local WIDTH = 380

local GOLD = { 1, 0.85, 0 }

-- Hidden tooltip used to ask the server for item data that isn't cached yet
local scanTip = CreateFrame("GameTooltip", "PocketVendorScanTip", UIParent, "GameTooltipTemplate")
scanTip:SetOwner(UIParent, "ANCHOR_NONE")

local function Solid(parent, layer, r, g, b, a)
    local tex = parent:CreateTexture(nil, layer)
    tex:SetTexture("Interface\\Buttons\\WHITE8X8")
    tex:SetVertexColor(r, g, b, a)
    return tex
end

local function SkinButton(btn)
    btn:SetNormalTexture("Interface\\Buttons\\WHITE8X8")
    btn:SetPushedTexture("Interface\\Buttons\\WHITE8X8")
    btn:SetHighlightTexture("Interface\\Buttons\\WHITE8X8")
    btn:GetNormalTexture():SetVertexColor(0.12, 0.12, 0.12, 0.9)
    btn:GetPushedTexture():SetVertexColor(0.18, 0.18, 0.18, 1)
    btn:GetHighlightTexture():SetVertexColor(0.25, 0.25, 0.25, 0.9)
    btn:GetFontString():SetTextColor(unpack(GOLD))
end

local function Count(set)
    local n = 0
    for _ in pairs(set) do n = n + 1 end
    return n
end

local function CursorItemId()
    local kind, id, link = GetCursorInfo()
    if kind ~= "item" then return nil end
    return tonumber(id) or addon:ToItemId(link)
end

---------------------------------------------------------------------
-- MAIN FRAME
---------------------------------------------------------------------
local function CreateMainFrame()
    if addon.UIFrame then return addon.UIFrame end

    local frame = CreateFrame("Frame", "PocketVendorFrame", UIParent)
    frame:SetSize(WIDTH, 470)
    frame:SetPoint("CENTER")
    frame:SetFrameStrata("DIALOG")
    frame:EnableMouse(true)
    frame:SetMovable(true)
    frame:SetClampedToScreen(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", frame.StartMoving)
    frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
    frame:Hide()
    tinsert(UISpecialFrames, "PocketVendorFrame") -- close with Escape

    Solid(frame, "BACKGROUND", 0.07, 0.07, 0.07, 0.95):SetAllPoints()

    local header = frame:CreateTexture(nil, "ARTWORK")
    header:SetPoint("TOPLEFT")
    header:SetPoint("TOPRIGHT")
    header:SetHeight(32)
    header:SetTexture("Interface\\PaperDollInfoFrame\\UI-GearManager-Title-Background")

    local icon = frame:CreateTexture(nil, "OVERLAY")
    icon:SetSize(18, 18)
    icon:SetPoint("TOPLEFT", 10, -7)
    icon:SetTexture("Interface\\Icons\\INV_Misc_Coin_02")

    local title = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightLarge")
    title:SetPoint("TOP", 0, -8)
    title:SetText("Pocket Vendor")

    local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", -2, -2)

    ---------------------------------------------------------------------
    -- TABS
    ---------------------------------------------------------------------
    local function MakeTab(x, text, listKey, tip)
        local btn = CreateFrame("Button", nil, frame)
        btn:SetSize(170, 24)
        btn:SetPoint("TOPLEFT", x, -40)
        btn.bg = Solid(btn, "BACKGROUND", 0.15, 0.15, 0.15, 0.7)
        btn.bg:SetAllPoints()

        btn.label = btn:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
        btn.label:SetPoint("CENTER")
        btn.label:SetText(text)

        function btn:SetActive(active)
            if active then
                self.bg:SetVertexColor(GOLD[1], GOLD[2], GOLD[3], 0.8)
                self.label:SetTextColor(1, 1, 1)
            else
                self.bg:SetVertexColor(0.15, 0.15, 0.15, 0.7)
                self.label:SetTextColor(0.8, 0.8, 0.8)
            end
        end

        btn:SetScript("OnClick", function()
            addon.activeTab = listKey
            addon:RefreshUI()
        end)
        btn:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine(text)
            GameTooltip:AddLine(tip, 1, 1, 1, true)
            GameTooltip:Show()
        end)
        btn:SetScript("OnLeave", GameTooltip_Hide)
        return btn
    end

    local tabSell = MakeTab(16, "Sell", "sell", "Sold automatically when you loot them, whatever their quality.")
    local tabKeep = MakeTab(WIDTH - 186, "Keep", "keep", "Never sold, not even grey items.")

    ---------------------------------------------------------------------
    -- LIST (FauxScrollFrame so it scrolls past the visible rows)
    ---------------------------------------------------------------------
    local listBg = Solid(frame, "BORDER", 0, 0, 0, 0.25)
    listBg:SetPoint("TOPLEFT", 16, -72)
    listBg:SetSize(WIDTH - 32, ROW_HEIGHT * VISIBLE_ROWS)

    local scroll = CreateFrame("ScrollFrame", "PocketVendorScroll", frame, "FauxScrollFrameTemplate")
    scroll:SetPoint("TOPLEFT", listBg)
    scroll:SetPoint("BOTTOMRIGHT", listBg, -24, 0)
    scroll:SetScript("OnVerticalScroll", function(self, offset)
        FauxScrollFrame_OnVerticalScroll(self, offset, ROW_HEIGHT, function() addon:RefreshUI() end)
    end)

    local empty = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    empty:SetPoint("CENTER", listBg)
    empty:SetWidth(WIDTH - 80)

    local rows = {}
    for i = 1, VISIBLE_ROWS do
        local row = CreateFrame("Button", nil, frame)
        row:SetSize(WIDTH - 56, ROW_HEIGHT)
        row:SetPoint("TOPLEFT", listBg, 0, -(i - 1) * ROW_HEIGHT)
        row:RegisterForClicks("LeftButtonUp", "RightButtonUp")

        row.bg = Solid(row, "BACKGROUND", 0, 0, 0, i % 2 == 0 and 0.22 or 0.1)
        row.bg:SetAllPoints()

        row.icon = row:CreateTexture(nil, "ARTWORK")
        row.icon:SetSize(18, 18)
        row.icon:SetPoint("LEFT", 6, 0)

        row.text = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        row.text:SetPoint("LEFT", row.icon, "RIGHT", 8, 0)
        row.text:SetPoint("RIGHT", -24, 0)
        row.text:SetJustifyH("LEFT")

        row.remove = row:CreateTexture(nil, "OVERLAY")
        row.remove:SetSize(12, 12)
        row.remove:SetPoint("RIGHT", -6, 0)
        row.remove:SetTexture("Interface\\Buttons\\UI-StopButton")
        row.remove:Hide()

        row:SetScript("OnClick", function(self, button)
            if not self.itemID then return end
            if button == "RightButton" then
                addon:RemoveItem(self.itemID)
            elseif IsModifiedClick("CHATLINK") then
                local _, link = GetItemInfo(self.itemID)
                if link then ChatEdit_InsertLink(link) end
            end
        end)
        row:SetScript("OnEnter", function(self)
            if not self.itemID then return end
            self.bg:SetVertexColor(0.3, 0.3, 0.3, 0.5)
            self.remove:Show()
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetHyperlink("item:" .. self.itemID)
            GameTooltip:AddLine("Right-click to remove", 0.6, 0.6, 0.6)
            GameTooltip:Show()
        end)
        row:SetScript("OnLeave", function(self)
            self.bg:SetVertexColor(0, 0, 0, i % 2 == 0 and 0.22 or 0.1)
            self.remove:Hide()
            GameTooltip:Hide()
        end)
        -- Dropping straight on the list works too
        row:SetScript("OnReceiveDrag", function()
            local id = CursorItemId()
            if id then
                addon:AddItem(id, addon.activeTab)
                ClearCursor()
            end
        end)

        rows[i] = row
    end

    ---------------------------------------------------------------------
    -- DROP ZONE
    ---------------------------------------------------------------------
    local drop = CreateFrame("Button", nil, frame)
    drop:SetSize(WIDTH - 32, 56)
    drop:SetPoint("TOPLEFT", listBg, "BOTTOMLEFT", 0, -10)
    drop.bg = Solid(drop, "BACKGROUND", 0.12, 0.12, 0.12, 0.8)
    drop.bg:SetAllPoints()

    drop.label = drop:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    drop.label:SetPoint("CENTER")

    local function OnDrop()
        local id = CursorItemId()
        if id then
            addon:AddItem(id, addon.activeTab)
            ClearCursor()
        end
    end
    drop:SetScript("OnReceiveDrag", OnDrop)
    drop:SetScript("OnClick", OnDrop)
    drop:SetScript("OnEnter", function(self)
        if GetCursorInfo() == "item" then self.bg:SetVertexColor(GOLD[1], GOLD[2], GOLD[3], 0.25) end
    end)
    drop:SetScript("OnLeave", function(self)
        self.bg:SetVertexColor(0.12, 0.12, 0.12, 0.8)
    end)

    ---------------------------------------------------------------------
    -- ADD BY LINK OR ID
    ---------------------------------------------------------------------
    local input = CreateFrame("EditBox", "PocketVendorInput", frame, "InputBoxTemplate")
    input:SetSize(WIDTH - 130, 22)
    input:SetPoint("TOPLEFT", drop, "BOTTOMLEFT", 6, -10)
    input:SetAutoFocus(false)
    input:SetFontObject("GameFontHighlightSmall")

    local hint = input:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("LEFT", 2, 0)
    hint:SetText("Item ID or shift-click an item")
    -- 3.3.5 has no SetShown, so toggle the hint by hand
    local function UpdateHint(self)
        if self:GetText() == "" and not self:HasFocus() then hint:Show() else hint:Hide() end
    end
    input:SetScript("OnTextChanged", UpdateHint)
    input:SetScript("OnEditFocusGained", function() hint:Hide() end)
    input:SetScript("OnEditFocusLost", UpdateHint)
    input:SetScript("OnEscapePressed", input.ClearFocus)

    local addBtn = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    addBtn:SetSize(80, 24)
    addBtn:SetPoint("LEFT", input, "RIGHT", 10, 0)
    addBtn:SetText("Add")
    SkinButton(addBtn)

    local function AddFromInput()
        local text = input:GetText()
        if text ~= "" then
            addon:AddItem(text, addon.activeTab)
            input:SetText("")
        end
        input:ClearFocus()
    end
    addBtn:SetScript("OnClick", AddFromInput)
    input:SetScript("OnEnterPressed", AddFromInput)

    -- Shift-clicking an item while the box has focus puts its link here
    hooksecurefunc("ChatEdit_InsertLink", function(link)
        if input:HasFocus() and link then
            input:SetText(link)
        end
    end)

    ---------------------------------------------------------------------
    -- FOOTER: auto-sell switch and server status
    ---------------------------------------------------------------------
    local autoSell = CreateFrame("CheckButton", "PocketVendorAutoSell", frame, "UICheckButtonTemplate")
    autoSell:SetSize(24, 24)
    autoSell:SetPoint("BOTTOMLEFT", 12, 12)
    _G[autoSell:GetName() .. "Text"]:SetText("Auto-sell on loot")
    autoSell:SetScript("OnClick", function(self)
        addon:SetAutoSell(self:GetChecked() and true or false)
    end)

    local status = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    status:SetPoint("BOTTOMRIGHT", -16, 18)

    ---------------------------------------------------------------------
    -- REFRESH
    ---------------------------------------------------------------------
    local waitingForItems = false
    local retry = CreateFrame("Frame")
    retry:Hide()
    retry:SetScript("OnUpdate", function(self, elapsed)
        self.t = (self.t or 0) + elapsed
        if self.t > 1 then
            self.t = 0
            self:Hide()
            addon:RefreshUI()
        end
    end)

    function addon:RefreshUI()
        if not frame:IsShown() then return end

        local ids = {}
        for id in pairs(self:GetList(self.activeTab)) do
            ids[#ids + 1] = id
        end

        local function NameOf(id) return GetItemInfo(id) or "" end
        table.sort(ids, function(a, b)
            local na, nb = NameOf(a), NameOf(b)
            if na == nb then return a < b end
            return na < nb
        end)

        FauxScrollFrame_Update(scroll, #ids, VISIBLE_ROWS, ROW_HEIGHT)
        local offset = FauxScrollFrame_GetOffset(scroll)

        waitingForItems = false
        for i, row in ipairs(rows) do
            local id = ids[i + offset]
            if id then
                local name, _, quality, _, _, _, _, _, _, texture = GetItemInfo(id)
                if not name then
                    waitingForItems = true
                    scanTip:SetHyperlink("item:" .. id)
                end
                local r, g, b = GetItemQualityColor(quality or 1)
                row.itemID = id
                row.icon:SetTexture(texture or "Interface\\Icons\\INV_Misc_QuestionMark")
                row.text:SetText(name or ("Item #" .. id))
                row.text:SetTextColor(r, g, b)
                row:Show()
            else
                row.itemID = nil
                row:Hide()
            end
        end

        if waitingForItems then retry:Show() end

        tabSell:SetActive(self.activeTab == "sell")
        tabKeep:SetActive(self.activeTab == "keep")
        tabSell.label:SetText(("Sell (%d)"):format(Count(self.sell)))
        tabKeep.label:SetText(("Keep (%d)"):format(Count(self.keep)))

        if #ids == 0 then
            empty:SetText(self.activeTab == "sell"
                and "Your sell list is empty.\nDrop items here to sell them when you loot them."
                or "Your keep list is empty.\nDrop items here to never sell them.")
            empty:Show()
        else
            empty:Hide()
        end

        drop.label:SetText(self.activeTab == "sell"
            and "Drop an item here to |cffffd700sell|r it on loot"
            or "Drop an item here to |cff00ff00keep|r it")

        autoSell:SetChecked(self.autoSell)
        if self.toggleAllowed and self.connected then autoSell:Enable() else autoSell:Disable() end

        if self.connected and not self.moduleEnabled then
            status:SetText("|cffff8040Disabled on this server|r")
        elseif self.connected then
            status:SetText("|cff00ff00Connected|r")
        elseif self.syncSentAt and GetTime() - self.syncSentAt > 5 then
            status:SetText("|cffff4040Server module not found|r")
        else
            status:SetText("|cffaaaaaaConnecting...|r")
        end
    end

    frame:SetScript("OnShow", function() addon:RefreshUI() end)

    addon.UIFrame = frame
    return frame
end

function addon:ToggleUI()
    local frame = self.UIFrame or CreateMainFrame()
    if frame:IsShown() then
        frame:Hide()
    else
        frame:Show()
        if not self.connected then self:Sync() end
    end
end

---------------------------------------------------------------------
-- BUTTON ON THE MERCHANT WINDOW
---------------------------------------------------------------------
local merchant = CreateFrame("Frame")
merchant:RegisterEvent("MERCHANT_SHOW")
merchant:SetScript("OnEvent", function()
    if PocketVendorMerchantButton then return end
    local btn = CreateFrame("Button", "PocketVendorMerchantButton", MerchantFrame, "UIPanelButtonTemplate")
    btn:SetSize(90, 22)
    -- Left of MyAutoVendor's "MyAV" button, so both fit when both addons are installed
    btn:SetPoint("TOPRIGHT", -124, -36)
    btn:SetText("Pocket Vendor")
    btn:SetScript("OnClick", function() addon:ToggleUI() end)
end)
