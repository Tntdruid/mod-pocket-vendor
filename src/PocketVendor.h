#ifndef MOD_POCKET_VENDOR_H
#define MOD_POCKET_VENDOR_H

#include "ItemTemplate.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <iterator>
#include <string>
#include <string_view>

// Shared by the auto-sell code (mod_pocket_vendor.cpp) and the supplies code
// (PocketVendorSupplies.cpp).
namespace PocketVendor
{
    constexpr char const* CHAT_PREFIX = "|cff00ccff[Pocket Vendor]|r ";

    // Addon messages arrive as "<prefix>\t<body>"
    constexpr char const* ADDON_PREFIX = "PVEND";

    inline bool IsBot(Player* player)
    {
        // Playerbots run on sessions without a network socket
        return player->GetSession()->GetRemoteAddress() == "headless";
    }

    // Sends one addon message; the client fires CHAT_MSG_ADDON with prefix PVEND
    inline void SendAddon(Player* player, std::string const& body)
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

    inline void SendAddonError(Player* player, std::string const& text)
    {
        SendAddon(player, "ERR:" + text);
    }

    // Formats money like the in-game vendor: 1[g] 25[s] 3[c] with coin icons
    inline std::string FormatMoney(uint32 money)
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
    inline std::string FormatItem(ItemTemplate const* proto, uint32 count)
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

    // Handles the supply shop's addon messages (SHOP, BUY:...).
    // Returns false when the message isn't a shop message.
    bool HandleSupplyMessage(Player* player, std::string_view body);
}

void AddPocketVendorSuppliesScripts();

#endif
