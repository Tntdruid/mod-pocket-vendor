#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <iterator>
#include <unordered_map>
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{
    enum AnnounceMode : uint32
    {
        ANNOUNCE_NONE     = 0,
        ANNOUNCE_PER_ITEM = 1,
        ANNOUNCE_SUMMARY  = 2
    };

    // Values stored in mod_pocket_vendor_list.list
    enum ListType : uint8
    {
        LIST_SELL = 0,
        LIST_KEEP = 1
    };

    struct PocketVendorConfig
    {
        bool enabled = true;
        uint32 announce = ANNOUNCE_SUMMARY;
        uint32 summaryDelay = 1000;
        bool sellCommonGear = false;
        bool playerToggle = true;
        bool playerbots = true;
        bool playerLists = true;
        uint32 maxListSize = 200;
        std::unordered_set<uint32> blacklist;
    };

    PocketVendorConfig sVendorConfig;

    // Per-character setting: index 0 holds 1 when the player turned auto-selling off
    constexpr char const* SETTING_SOURCE = "mod-pocket-vendor";
    constexpr uint32 SETTING_DISABLED = 0;

    constexpr char const* SUMMARY_KEY = "PocketVendor.Summary";
    constexpr char const* LISTS_KEY = "PocketVendor.Lists";
    constexpr char const* PREFIX = "|cff00ccff[Pocket Vendor]|r ";

    // Addon messages arrive as "<prefix>\t<body>"
    constexpr char const* ADDON_PREFIX = "PVEND";
    constexpr std::size_t ADDON_CHUNK = 200;

    // Sales collected for one summary line, stored on the player
    struct VendorSummary : public DataMap::Base
    {
        uint32 items = 0;
        uint32 money = 0;
        int32 timer = 0;
        std::string lastLink;
    };

    // The player's personal sell/keep lists, loaded at login
    struct PlayerLists : public DataMap::Base
    {
        std::unordered_map<uint32, uint8> items; // item id -> ListType
    };

    // Formats money like the in-game vendor: 1[g] 25[s] 3[c] with coin icons
    std::string FormatMoney(uint32 money)
    {
        static constexpr char const* GOLD_ICON   = "|TInterface\\MoneyFrame\\UI-GoldIcon:0:0:2:0|t";
        static constexpr char const* SILVER_ICON = "|TInterface\\MoneyFrame\\UI-SilverIcon:0:0:2:0|t";
        static constexpr char const* COPPER_ICON = "|TInterface\\MoneyFrame\\UI-CopperIcon:0:0:2:0|t";

        uint32 gold = money / GOLD;
        uint32 silver = (money % GOLD) / SILVER;
        uint32 copper = money % SILVER;

        std::string result;
        if (gold > 0)
        {
            result += Acore::StringFormat("|cffffd700{}|r{} ", gold, GOLD_ICON);
        }
        if (silver > 0)
        {
            result += Acore::StringFormat("|cffc7c7cf{}|r{} ", silver, SILVER_ICON);
        }
        if (copper > 0 || result.empty())
        {
            result += Acore::StringFormat("|cffeda55f{}|r{} ", copper, COPPER_ICON);
        }

        result.pop_back(); // trailing space
        return result;
    }

    // Clickable item link in the item's quality colour, e.g. [Broken Fang] x3
    std::string FormatItem(ItemTemplate const* proto, uint32 count)
    {
        static constexpr char const* QUALITY_COLORS[] = { "9d9d9d", "ffffff", "1eff00", "0070dd", "a335ee", "ff8000", "e6cc80", "e6cc80" };
        char const* color = proto->Quality < std::size(QUALITY_COLORS) ? QUALITY_COLORS[proto->Quality] : "ffffff";

        std::string link = Acore::StringFormat("|cff{}|Hitem:{}::::::::80:::::|h[{}]|h|r", color, proto->ItemId, proto->Name1);
        if (count > 1)
        {
            link += Acore::StringFormat(" |cffffffffx{}|r", count);
        }
        return link;
    }

    bool IsBot(Player* player)
    {
        // Playerbots run on sessions without a network socket
        return player->GetSession()->GetRemoteAddress() == "headless";
    }

    bool IsAutoSellEnabledFor(Player* player)
    {
        if (!sVendorConfig.playerToggle)
        {
            return true;
        }
        return player->GetPlayerSetting(SETTING_SOURCE, SETTING_DISABLED).value == 0;
    }

    // Returns the list an item is on, or nullptr when it is on neither
    uint8 const* FindOnPlayerList(Player* player, uint32 itemId)
    {
        if (!sVendorConfig.playerLists)
        {
            return nullptr;
        }

        PlayerLists* lists = player->CustomData.Get<PlayerLists>(LISTS_KEY);
        if (!lists)
        {
            return nullptr;
        }

        auto it = lists->items.find(itemId);
        return it != lists->items.end() ? &it->second : nullptr;
    }

    bool IsSellableGear(ItemTemplate const* proto)
    {
        if (proto->InventoryType == INVTYPE_BODY || proto->InventoryType == INVTYPE_TABARD)
        {
            return false;
        }

        switch (proto->Class)
        {
            case ITEM_CLASS_WEAPON:
                return proto->SubClass != ITEM_SUBCLASS_WEAPON_FISHING_POLE &&
                       proto->SubClass != ITEM_SUBCLASS_WEAPON_MISC;
            case ITEM_CLASS_ARMOR:
                return proto->SubClass == ITEM_SUBCLASS_ARMOR_CLOTH ||
                       proto->SubClass == ITEM_SUBCLASS_ARMOR_LEATHER ||
                       proto->SubClass == ITEM_SUBCLASS_ARMOR_MAIL ||
                       proto->SubClass == ITEM_SUBCLASS_ARMOR_PLATE ||
                       proto->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD;
            default:
                return false;
        }
    }

    // Items that must never be sold, whatever list they are on
    bool IsProtected(Player* player, ItemTemplate const* proto)
    {
        // Worthless items stay in the bags instead of being destroyed for nothing
        if (proto->SellPrice == 0)
        {
            return true;
        }

        // Quest items, items that start a quest, or that an active quest needs
        if (proto->Class == ITEM_CLASS_QUEST || proto->StartQuest || player->HasQuestForItem(proto->ItemId))
        {
            return true;
        }

        return sVendorConfig.blacklist.count(proto->ItemId) != 0;
    }

    bool ShouldSell(Player* player, ItemTemplate const* proto)
    {
        if (IsProtected(player, proto))
        {
            return false;
        }

        // The player's own lists win over the automatic rules
        if (uint8 const* list = FindOnPlayerList(player, proto->ItemId))
        {
            return *list == LIST_SELL;
        }

        if (proto->Quality == ITEM_QUALITY_POOR)
        {
            return true;
        }

        return sVendorConfig.sellCommonGear && proto->Quality == ITEM_QUALITY_NORMAL && IsSellableGear(proto);
    }

    void LoadPlayerLists(Player* player)
    {
        PlayerLists* lists = player->CustomData.GetDefault<PlayerLists>(LISTS_KEY);
        lists->items.clear();

        QueryResult result = CharacterDatabase.Query("SELECT `item_id`, `list` FROM `mod_pocket_vendor_list` WHERE `guid` = {}", player->GetGUID().GetCounter());
        if (!result)
        {
            return;
        }

        do
        {
            Field* fields = result->Fetch();
            lists->items[fields[0].Get<uint32>()] = fields[1].Get<uint8>();
        } while (result->NextRow());
    }

    // ------------------------------------------------------------------
    // Addon communication
    // ------------------------------------------------------------------

    // Sends one addon message; the client fires CHAT_MSG_ADDON with prefix PVEND
    void SendAddon(Player* player, std::string const& body)
    {
        std::string msg = Acore::StringFormat("{}\t{}", ADDON_PREFIX, body);

        WorldPacket data(SMSG_MESSAGECHAT, 1 + 4 + 8 + 4 + 8 + 4 + msg.size() + 1 + 1);
        data << uint8(CHAT_MSG_WHISPER);
        data << uint32(LANG_ADDON);
        data << uint64(0);
        data << uint32(0);
        data << uint64(0);
        data << uint32(msg.size() + 1);
        data << msg;
        data << uint8(0);
        player->GetSession()->SendPacket(&data);
    }

    void SendState(Player* player)
    {
        SendAddon(player, Acore::StringFormat("STATE:{}:{}:{}",
            sVendorConfig.enabled ? 1 : 0,
            IsAutoSellEnabledFor(player) ? 1 : 0,
            sVendorConfig.playerToggle ? 1 : 0));
    }

    // Sends the whole list in chunks: LIST:BEGIN, LIST:S:1,2,3, LIST:K:4,5, LIST:END
    void SendLists(Player* player)
    {
        SendAddon(player, "LIST:BEGIN");

        if (PlayerLists* lists = player->CustomData.Get<PlayerLists>(LISTS_KEY))
        {
            for (uint8 type : { LIST_SELL, LIST_KEEP })
            {
                std::string chunk;
                for (auto const& [itemId, list] : lists->items)
                {
                    if (list != type)
                    {
                        continue;
                    }

                    if (!chunk.empty() && chunk.size() > ADDON_CHUNK)
                    {
                        SendAddon(player, Acore::StringFormat("LIST:{}:{}", type == LIST_SELL ? 'S' : 'K', chunk));
                        chunk.clear();
                    }

                    if (!chunk.empty())
                    {
                        chunk += ',';
                    }
                    chunk += std::to_string(itemId);
                }

                if (!chunk.empty())
                {
                    SendAddon(player, Acore::StringFormat("LIST:{}:{}", type == LIST_SELL ? 'S' : 'K', chunk));
                }
            }
        }

        SendAddon(player, "LIST:END");
    }

    void SendError(Player* player, std::string const& text)
    {
        SendAddon(player, "ERR:" + text);
    }

    void HandleAdd(Player* player, char type, std::string_view idText)
    {
        Optional<uint32> itemId = Acore::StringTo<uint32>(idText);
        ItemTemplate const* proto = itemId ? sObjectMgr->GetItemTemplate(*itemId) : nullptr;
        if (!proto)
        {
            SendError(player, "Unknown item.");
            return;
        }

        uint8 list = type == 'S' ? LIST_SELL : LIST_KEEP;
        if (list == LIST_SELL)
        {
            if (proto->SellPrice == 0)
            {
                SendError(player, Acore::StringFormat("{} has no vendor price.", proto->Name1));
                return;
            }
            if (proto->Class == ITEM_CLASS_QUEST || proto->StartQuest)
            {
                SendError(player, Acore::StringFormat("{} is a quest item and is never sold.", proto->Name1));
                return;
            }
        }

        PlayerLists* lists = player->CustomData.GetDefault<PlayerLists>(LISTS_KEY);
        if (!lists->items.count(*itemId) && lists->items.size() >= sVendorConfig.maxListSize)
        {
            SendError(player, Acore::StringFormat("Your lists are full ({} items).", sVendorConfig.maxListSize));
            return;
        }

        // An item lives on one list only, so adding it moves it
        lists->items[*itemId] = list;
        CharacterDatabase.Execute("REPLACE INTO `mod_pocket_vendor_list` (`guid`, `item_id`, `list`) VALUES ({}, {}, {})",
            player->GetGUID().GetCounter(), *itemId, list);

        SendAddon(player, Acore::StringFormat("ADDED:{}:{}", type, *itemId));
    }

    void HandleRemove(Player* player, std::string_view idText)
    {
        Optional<uint32> itemId = Acore::StringTo<uint32>(idText);
        if (!itemId)
        {
            return;
        }

        if (PlayerLists* lists = player->CustomData.Get<PlayerLists>(LISTS_KEY))
        {
            lists->items.erase(*itemId);
        }

        CharacterDatabase.Execute("DELETE FROM `mod_pocket_vendor_list` WHERE `guid` = {} AND `item_id` = {}",
            player->GetGUID().GetCounter(), *itemId);

        SendAddon(player, Acore::StringFormat("REMOVED:{}", *itemId));
    }

    void HandleToggle(Player* player, std::string_view value)
    {
        if (sVendorConfig.playerToggle)
        {
            player->UpdatePlayerSetting(SETTING_SOURCE, SETTING_DISABLED, value == "1" ? 0 : 1);
        }
        SendState(player);
    }

    // Body formats: SYNC | ADD:S:<id> | ADD:K:<id> | DEL:<id> | TOGGLE:<0|1>
    void HandleAddonMessage(Player* player, std::string_view body)
    {
        if (body == "SYNC")
        {
            SendState(player);
            SendLists(player);
            return;
        }

        if (body.rfind("TOGGLE:", 0) == 0)
        {
            HandleToggle(player, body.substr(7));
            return;
        }

        if (!sVendorConfig.playerLists)
        {
            SendError(player, "Personal lists are disabled on this server.");
            return;
        }

        if (body.size() > 6 && body.rfind("ADD:", 0) == 0 && (body[4] == 'S' || body[4] == 'K') && body[5] == ':')
        {
            HandleAdd(player, body[4], body.substr(6));
        }
        else if (body.rfind("DEL:", 0) == 0)
        {
            HandleRemove(player, body.substr(4));
        }
    }

    // Returns the body when msg is one of our addon messages, or an empty view
    std::string_view GetAddonBody(uint32 type, uint32 lang, std::string const& msg)
    {
        if (lang != LANG_ADDON || type != CHAT_MSG_WHISPER)
        {
            return {};
        }

        std::string_view view(msg);
        std::size_t prefixLen = std::char_traits<char>::length(ADDON_PREFIX);
        if (view.size() <= prefixLen + 1 || view.compare(0, prefixLen, ADDON_PREFIX) != 0 || view[prefixLen] != '\t')
        {
            return {};
        }

        return view.substr(prefixLen + 1);
    }
}

class PocketVendor : public PlayerScript
{
public:
    PocketVendor() : PlayerScript("PocketVendor", {
        PLAYERHOOK_ON_LOOT_ITEM,
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_DELETE_FROM_DB,
        PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT
    }) {}

    void OnPlayerLogin(Player* player) override
    {
        if (sVendorConfig.playerLists && !IsBot(player))
        {
            LoadPlayerLists(player);
        }
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `mod_pocket_vendor_list` WHERE `guid` = {}", guid);
    }

    // Act on addon messages from the PocketVendor addon
    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
    {
        std::string_view body = GetAddonBody(type, lang, msg);
        if (!body.empty() && player)
        {
            HandleAddonMessage(player, body);
        }
    }

    // ...and stop the whisper-to-self from being delivered back to the client
    bool OnPlayerCanUseChat(Player* /*player*/, uint32 type, uint32 lang, std::string& msg, Player* /*receiver*/) override
    {
        return GetAddonBody(type, lang, msg).empty();
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid /*lootguid*/) override
    {
        if (!sVendorConfig.enabled || !player || !item || !item->GetTemplate())
        {
            return;
        }

        bool bot = IsBot(player);
        if (bot && !sVendorConfig.playerbots)
        {
            return;
        }

        if (!IsAutoSellEnabledFor(player))
        {
            return;
        }

        ItemTemplate const* proto = item->GetTemplate();
        if (!ShouldSell(player, proto))
        {
            return;
        }

        // The looted item may have merged into an existing stack, so sell the whole stack
        uint32 stackCount = item->GetCount();
        uint32 money = proto->SellPrice * stackCount;

        // At the gold cap the money can't be added, so keep the item
        if (!player->ModifyMoney(static_cast<int32>(money), false))
        {
            return;
        }

        player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);

        if (!bot)
        {
            Announce(player, proto, stackCount, money);
        }
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        VendorSummary* summary = player->CustomData.Get<VendorSummary>(SUMMARY_KEY);
        if (!summary)
        {
            return;
        }

        summary->timer -= static_cast<int32>(diff);
        if (summary->timer > 0)
        {
            return;
        }

        std::string message;
        if (summary->items == 1)
        {
            message = Acore::StringFormat("{}{} |cffaaaaaasold for|r {}", PREFIX, summary->lastLink, FormatMoney(summary->money));
        }
        else
        {
            message = Acore::StringFormat("{}|cffaaaaaaSold|r |cffffffff{}|r |cffaaaaaaitems for|r {}", PREFIX, summary->items, FormatMoney(summary->money));
        }

        ChatHandler(player->GetSession()).SendSysMessage(message);
        player->CustomData.Erase(SUMMARY_KEY);
    }

    void OnPlayerLogout(Player* player) override
    {
        player->CustomData.Erase(SUMMARY_KEY);
        player->CustomData.Erase(LISTS_KEY);
    }

private:
    void Announce(Player* player, ItemTemplate const* proto, uint32 count, uint32 money)
    {
        switch (sVendorConfig.announce)
        {
            case ANNOUNCE_PER_ITEM:
            {
                std::string message = Acore::StringFormat("{}{} |cffaaaaaasold for|r {}", PREFIX, FormatItem(proto, count), FormatMoney(money));
                ChatHandler(player->GetSession()).SendSysMessage(message);
                break;
            }
            case ANNOUNCE_SUMMARY:
            {
                VendorSummary* summary = player->CustomData.GetDefault<VendorSummary>(SUMMARY_KEY);
                summary->items += count;
                summary->money += money;
                summary->timer = static_cast<int32>(sVendorConfig.summaryDelay);
                summary->lastLink = FormatItem(proto, count);
                break;
            }
            default:
                break;
        }
    }
};

class PocketVendorCommands : public CommandScript
{
public:
    PocketVendorCommands() : CommandScript("PocketVendorCommands") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable vendorCommandTable =
        {
            { "on",  HandleVendorOn,     SEC_PLAYER, Console::No },
            { "off", HandleVendorOff,    SEC_PLAYER, Console::No },
            { "",    HandleVendorStatus, SEC_PLAYER, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "vendor", vendorCommandTable }
        };

        return commandTable;
    }

    static bool HandleVendorOn(ChatHandler* handler)
    {
        return SetEnabled(handler, true);
    }

    static bool HandleVendorOff(ChatHandler* handler)
    {
        return SetEnabled(handler, false);
    }

    static bool HandleVendorStatus(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        bool enabled = sVendorConfig.enabled && IsAutoSellEnabledFor(player);

        handler->SendSysMessage(Acore::StringFormat("{}Auto-selling is {}. Use |cffffffff.vendor on|r or |cffffffff.vendor off|r.",
            PREFIX, enabled ? "|cff00ff00on|r" : "|cffff0000off|r"));
        return true;
    }

private:
    static bool SetEnabled(ChatHandler* handler, bool enabled)
    {
        if (!sVendorConfig.enabled)
        {
            handler->SendSysMessage(Acore::StringFormat("{}The module is disabled on this server.", PREFIX));
            return true;
        }

        if (!sVendorConfig.playerToggle)
        {
            handler->SendSysMessage(Acore::StringFormat("{}Auto-selling can't be turned off on this server.", PREFIX));
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        player->UpdatePlayerSetting(SETTING_SOURCE, SETTING_DISABLED, enabled ? 0 : 1);

        handler->SendSysMessage(Acore::StringFormat("{}Auto-selling is now {}.",
            PREFIX, enabled ? "|cff00ff00on|r" : "|cffff0000off|r"));

        // Keep an open addon window in step with the command
        SendState(player);
        return true;
    }
};

class PocketVendorWorld : public WorldScript
{
public:
    PocketVendorWorld() : WorldScript("PocketVendorWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sVendorConfig.enabled = sConfigMgr->GetOption<bool>("PocketVendor.Enable", true);
        sVendorConfig.announce = sConfigMgr->GetOption<uint32>("PocketVendor.Announce", ANNOUNCE_SUMMARY);
        sVendorConfig.summaryDelay = sConfigMgr->GetOption<uint32>("PocketVendor.SummaryDelay", 1000);
        sVendorConfig.sellCommonGear = sConfigMgr->GetOption<bool>("PocketVendor.SellCommonGear", false);
        sVendorConfig.playerToggle = sConfigMgr->GetOption<bool>("PocketVendor.PlayerToggle", true);
        sVendorConfig.playerbots = sConfigMgr->GetOption<bool>("PocketVendor.Playerbots", true);
        sVendorConfig.playerLists = sConfigMgr->GetOption<bool>("PocketVendor.PlayerLists", true);
        sVendorConfig.maxListSize = sConfigMgr->GetOption<uint32>("PocketVendor.MaxListSize", 200);

        sVendorConfig.blacklist.clear();
        std::string blacklist = sConfigMgr->GetOption<std::string>("PocketVendor.Blacklist", "");
        for (std::string_view entry : Acore::Tokenize(blacklist, ',', false))
        {
            // Allow "1, 2, 3" as well as "1,2,3"
            while (!entry.empty() && entry.front() == ' ')
            {
                entry.remove_prefix(1);
            }
            while (!entry.empty() && entry.back() == ' ')
            {
                entry.remove_suffix(1);
            }

            if (Optional<uint32> itemId = Acore::StringTo<uint32>(entry))
            {
                sVendorConfig.blacklist.insert(*itemId);
            }
        }
    }
};

void Addmod_pocket_vendorScripts()
{
    new PocketVendor();
    new PocketVendorCommands();
    new PocketVendorWorld();
}
