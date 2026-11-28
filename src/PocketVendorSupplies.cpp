// Supplies: bots buy their own ammo, reagents, potions and food, and players
// can buy the same things from the Shop tab of the PocketVendor addon.
//
// Nothing is free. Every purchase costs the item's vendor price (times
// PocketVendor.Shop.PriceRate), paid from the character's own gold.
//
// The catalog is built once at startup:
//   - Ammo, food and drink: items sold for gold by any vendor (npc_vendor).
//   - Reagents: vendor-sold items that a spell the player knows uses as a reagent.
//   - Potions: no vendor sells them, so healing and mana potions come from the
//     Alchemy recipes instead.
// Each player is offered the best item of each kind for their level.

#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupReference.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PocketVendor.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <vector>

namespace
{
    using PocketVendor::FormatItem;
    using PocketVendor::FormatMoney;
    using PocketVendor::IsBot;
    using PocketVendor::SendAddon;
    using PocketVendor::SendAddonError;

    // Kinds of supplies; one best item per kind is offered
    enum SupplyKind : uint8
    {
        KIND_ARROW,
        KIND_BULLET,
        KIND_HEALING_POTION,
        KIND_MANA_POTION,
        KIND_FOOD,
        KIND_DRINK,
        MAX_SUPPLY_KINDS
    };

    // Shop categories sent to the addon
    constexpr char CATEGORY_AMMO = 'A';
    constexpr char CATEGORY_REAGENT = 'R';
    constexpr char CATEGORY_POTION = 'P';
    constexpr char CATEGORY_FOOD = 'F';

    constexpr uint32 MAX_LOTS_PER_BUY = 100;
    constexpr char const* BOT_TIMER_KEY = "PocketVendor.BotSupplies";

    struct SuppliesConfig
    {
        bool shopEnabled = true;
        uint32 priceRate = 100;

        bool botsEnabled = true;
        uint32 botInterval = 300000;
        uint32 botGoldReserve = 10000;
        uint32 botAmmo = 1000;
        uint32 botReagents = 20;
        uint32 botPotions = 5;
        uint32 botFood = 0;
        uint32 botDrink = 0;
        bool botLog = false;
        bool botTellGroup = true;
        uint32 botRefillPercent = 25;
    };

    SuppliesConfig sSuppliesConfig;

    // Built at startup, read-only afterwards
    std::array<std::vector<ItemTemplate const*>, MAX_SUPPLY_KINDS> sCatalog;
    std::unordered_set<uint32> sVendorItems;

    // Created on a player's first update; non-bots keep isBot = false and are skipped
    struct BotSupplyTimer : public DataMap::Base
    {
        bool isBot = false;
        int32 timer = 0;
    };

    struct Offer
    {
        ItemTemplate const* proto;
        char category;
    };

    // ------------------------------------------------------------------
    // Catalog
    // ------------------------------------------------------------------

    // The only non-empty effect of the item's use spell, or nullptr when it has several
    SpellEffectInfo const* SingleUseEffect(ItemTemplate const* proto)
    {
        SpellInfo const* spell = proto->Spells[0].SpellId > 0 ? sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId) : nullptr;
        if (!spell)
        {
            return nullptr;
        }

        SpellEffectInfo const* found = nullptr;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (!spell->Effects[i].Effect)
            {
                continue;
            }
            if (found)
            {
                return nullptr;
            }
            found = &spell->Effects[i];
        }
        return found;
    }

    // Food restores health over time, drink restores mana. The spell may carry other
    // effects too (drinks have a periodic dummy aura for arena), so only the regen
    // auras decide; something that restores both is neither.
    void AddFoodOrDrink(ItemTemplate const* proto)
    {
        SpellInfo const* spell = proto->Spells[0].SpellId > 0 ? sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId) : nullptr;
        if (!spell)
        {
            return;
        }

        bool health = false;
        bool mana = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& effect = spell->Effects[i];
            if (effect.Effect != SPELL_EFFECT_APPLY_AURA)
            {
                continue;
            }
            if (effect.ApplyAuraName == SPELL_AURA_MOD_REGEN)
            {
                health = true;
            }
            else if (effect.ApplyAuraName == SPELL_AURA_MOD_POWER_REGEN && effect.MiscValue == POWER_MANA)
            {
                mana = true;
            }
        }

        if (health && !mana)
        {
            sCatalog[KIND_FOOD].push_back(proto);
        }
        else if (mana && !health)
        {
            sCatalog[KIND_DRINK].push_back(proto);
        }
    }

    void AddPotion(ItemTemplate const* proto)
    {
        SpellEffectInfo const* effect = SingleUseEffect(proto);
        if (!effect)
        {
            return;
        }

        if (effect->Effect == SPELL_EFFECT_HEAL)
        {
            sCatalog[KIND_HEALING_POTION].push_back(proto);
        }
        else if (effect->Effect == SPELL_EFFECT_ENERGIZE && effect->MiscValue == POWER_MANA)
        {
            sCatalog[KIND_MANA_POTION].push_back(proto);
        }
    }

    void BuildCatalog()
    {
        for (auto& kind : sCatalog)
        {
            kind.clear();
        }
        sVendorItems.clear();

        // Items sold for gold, in unlimited supply, by any vendor
        if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT `item` FROM `npc_vendor` WHERE `item` > 0 AND `maxcount` = 0 AND `ExtendedCost` = 0"))
        {
            do
            {
                sVendorItems.insert(result->Fetch()[0].Get<uint32>());
            } while (result->NextRow());
        }

        for (uint32 itemId : sVendorItems)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto || proto->BuyPrice == 0)
            {
                continue;
            }

            // Rare reputation ammo is left out; it costs far more than the level asks for
            if (proto->Class == ITEM_CLASS_PROJECTILE && proto->Quality <= ITEM_QUALITY_UNCOMMON)
            {
                if (proto->SubClass == ITEM_SUBCLASS_ARROW)
                {
                    sCatalog[KIND_ARROW].push_back(proto);
                }
                else if (proto->SubClass == ITEM_SUBCLASS_BULLET)
                {
                    sCatalog[KIND_BULLET].push_back(proto);
                }
            }
            else if (proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_FOOD)
            {
                AddFoodOrDrink(proto);
            }
        }

        // Potions made by Alchemy
        std::unordered_set<uint32> potions;
        for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        {
            SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
            if (!ability || ability->SkillLine != SKILL_ALCHEMY)
            {
                continue;
            }

            SpellInfo const* recipe = sSpellMgr->GetSpellInfo(ability->Spell);
            if (!recipe)
            {
                continue;
            }

            for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
            {
                if (recipe->Effects[e].Effect != SPELL_EFFECT_CREATE_ITEM)
                {
                    continue;
                }

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(recipe->Effects[e].ItemType);
                if (proto && proto->IsPotion() && proto->Quality == ITEM_QUALITY_NORMAL && proto->BuyPrice > 0 && potions.insert(proto->ItemId).second)
                {
                    AddPotion(proto);
                }
            }
        }

        for (auto& kind : sCatalog)
        {
            std::sort(kind.begin(), kind.end(), [](ItemTemplate const* a, ItemTemplate const* b)
            {
                if (a->RequiredLevel != b->RequiredLevel)
                {
                    return a->RequiredLevel < b->RequiredLevel;
                }
                if (a->ItemLevel != b->ItemLevel)
                {
                    return a->ItemLevel < b->ItemLevel;
                }
                return a->ItemId < b->ItemId;
            });
        }
    }

    bool CanUse(Player* player, ItemTemplate const* proto)
    {
        if (proto->RequiredLevel > player->GetLevel())
        {
            return false;
        }
        if (!(proto->AllowableClass & player->getClassMask()) || !(proto->AllowableRace & player->getRaceMask()))
        {
            return false;
        }
        if (proto->RequiredReputationFaction && uint32(player->GetReputationRank(proto->RequiredReputationFaction)) < proto->RequiredReputationRank)
        {
            return false;
        }
        return true;
    }

    // Best usable item of a kind: the highest required level the player meets
    ItemTemplate const* BestFor(Player* player, SupplyKind kind)
    {
        auto const& items = sCatalog[kind];
        for (auto it = items.rbegin(); it != items.rend(); ++it)
        {
            if (CanUse(player, *it))
            {
                return *it;
            }
        }
        return nullptr;
    }

    bool UsesMana(Player* player)
    {
        return player->GetMaxPower(POWER_MANA) > 0;
    }

    // True for spells from a class skill line (buffs, Reincarnation, teleports, ...),
    // false for profession recipes such as cooking, whose reagents aren't supplies
    bool IsClassSpell(uint32 spellId)
    {
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            SkillLineEntry const* skill = sSkillLineStore.LookupEntry(it->second->SkillLine);
            if (skill && skill->categoryId == SKILL_CATEGORY_CLASS)
            {
                return true;
            }
        }
        return false;
    }

    // Vendor-sold reagents used by class spells the player knows
    std::vector<ItemTemplate const*> ReagentsFor(Player* player)
    {
        std::unordered_set<uint32> ids;
        for (auto const& [spellId, spell] : player->GetSpellMap())
        {
            if (spell->State == PLAYERSPELL_REMOVED || !spell->Active || !IsClassSpell(spellId))
            {
                continue;
            }

            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info)
            {
                continue;
            }

            for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                if (info->Reagent[i] > 0 && info->ReagentCount[i] > 0 && sVendorItems.count(info->Reagent[i]))
                {
                    ids.insert(info->Reagent[i]);
                }
            }
        }

        std::vector<ItemTemplate const*> reagents;
        for (uint32 id : ids)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id);
            if (proto && proto->BuyPrice > 0 && CanUse(player, proto))
            {
                reagents.push_back(proto);
            }
        }
        return reagents;
    }

    // The ammo kind for the ranged weapon the player has equipped, if any
    Optional<SupplyKind> EquippedAmmoKind(Player* player)
    {
        Item* ranged = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!ranged || ranged->GetTemplate()->Class != ITEM_CLASS_WEAPON)
        {
            return {};
        }

        switch (ranged->GetTemplate()->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                return KIND_ARROW;
            case ITEM_SUBCLASS_WEAPON_GUN:
                return KIND_BULLET;
            default:
                return {};
        }
    }

    // Everything the shop offers this player
    std::vector<Offer> OffersFor(Player* player)
    {
        std::vector<Offer> offers;
        auto add = [&](SupplyKind kind, char category)
        {
            if (ItemTemplate const* proto = BestFor(player, kind))
            {
                offers.push_back({ proto, category });
            }
        };

        // Ammo for every ranged weapon type the player can use, not just the equipped one
        if (player->HasSkill(SKILL_BOWS) || player->HasSkill(SKILL_CROSSBOWS))
        {
            add(KIND_ARROW, CATEGORY_AMMO);
        }
        if (player->HasSkill(SKILL_GUNS))
        {
            add(KIND_BULLET, CATEGORY_AMMO);
        }

        for (ItemTemplate const* reagent : ReagentsFor(player))
        {
            offers.push_back({ reagent, CATEGORY_REAGENT });
        }

        add(KIND_HEALING_POTION, CATEGORY_POTION);
        if (UsesMana(player))
        {
            add(KIND_MANA_POTION, CATEGORY_POTION);
        }

        add(KIND_FOOD, CATEGORY_FOOD);
        add(KIND_DRINK, CATEGORY_FOOD);
        return offers;
    }

    // ------------------------------------------------------------------
    // Buying
    // ------------------------------------------------------------------

    uint32 LotSize(ItemTemplate const* proto)
    {
        return std::max<uint32>(proto->BuyCount, 1);
    }

    uint32 LotPrice(ItemTemplate const* proto)
    {
        uint64 price = uint64(proto->BuyPrice) * sSuppliesConfig.priceRate / 100;
        return uint32(std::clamp<uint64>(price, 1, MAX_MONEY_AMOUNT));
    }

    enum class BuyResult
    {
        OK,
        NO_MONEY,
        NO_SPACE
    };

    // Buys whole lots (e.g. 200 arrows) and keeps reserve copper untouched
    BuyResult Buy(Player* player, ItemTemplate const* proto, uint32 lots, uint32 reserve, uint32& boughtCount, uint32& cost)
    {
        boughtCount = 0;
        cost = 0;

        lots = std::min(lots, MAX_LOTS_PER_BUY);
        uint32 price = LotPrice(proto);
        uint32 money = player->GetMoney();
        if (money <= reserve || price > money - reserve)
        {
            return BuyResult::NO_MONEY;
        }

        // Buy fewer lots when the money doesn't stretch to all of them
        lots = std::min<uint32>(lots, (money - reserve) / price);

        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, proto->ItemId, lots * LotSize(proto)) != EQUIP_ERR_OK)
        {
            return BuyResult::NO_SPACE;
        }

        Item* item = player->StoreNewItem(dest, proto->ItemId, true);
        if (!item)
        {
            return BuyResult::NO_SPACE;
        }

        boughtCount = lots * LotSize(proto);
        cost = lots * price;
        player->ModifyMoney(-int32(cost));
        player->SendNewItem(item, boughtCount, true, false, false, false);
        return BuyResult::OK;
    }

    // What a bot bought on one shopping trip
    struct ShoppingTrip
    {
        std::vector<std::pair<ItemTemplate const*, uint32>> items; // item, count
        uint32 cost = 0;
    };

    // Tops a bot up to target pieces of an item
    void TopUp(Player* bot, ItemTemplate const* proto, uint32 target, ShoppingTrip& trip)
    {
        if (!proto || target == 0)
        {
            return;
        }

        // Only shop once the stock is down to RefillPercent of the target (always below
        // 1 left), then buy all the way back up, so bots make few, big trips
        uint32 have = bot->GetItemCount(proto->ItemId);
        uint32 refillAt = std::max<uint32>(target * sSuppliesConfig.botRefillPercent / 100, 1);
        if (have >= target || have >= refillAt)
        {
            return;
        }

        uint32 lots = (target - have + LotSize(proto) - 1) / LotSize(proto);
        uint32 bought = 0;
        uint32 cost = 0;
        BuyResult result = Buy(bot, proto, lots, sSuppliesConfig.botGoldReserve, bought, cost);
        if (result == BuyResult::OK)
        {
            trip.items.emplace_back(proto, bought);
            trip.cost += cost;
        }

        if (!sSuppliesConfig.botLog)
        {
            return;
        }

        switch (result)
        {
            case BuyResult::OK:
                LOG_INFO("module", "Pocket Vendor: {} bought {} {} for {} copper ({} copper left).",
                    bot->GetName(), bought, proto->Name1, cost, bot->GetMoney());
                break;
            case BuyResult::NO_MONEY:
                LOG_INFO("module", "Pocket Vendor: {} needs {} {} but can't afford it ({} copper, {} kept in reserve).",
                    bot->GetName(), target - have, proto->Name1, bot->GetMoney(), sSuppliesConfig.botGoldReserve);
                break;
            case BuyResult::NO_SPACE:
                LOG_INFO("module", "Pocket Vendor: {} needs {} {} but has no bag space.",
                    bot->GetName(), target - have, proto->Name1);
                break;
        }
    }

    // Tells the real players in the bot's party or raid what it bought, in a few
    // lines of at most ITEMS_PER_LINE item links so the chat lines stay short
    void TellGroup(Player* bot, ShoppingTrip const& trip)
    {
        Group* group = bot->GetGroup();
        if (!group || trip.items.empty())
        {
            return;
        }

        constexpr std::size_t ITEMS_PER_LINE = 4;
        std::vector<std::string> lines;
        std::string botName = Acore::StringFormat("|Hplayer:{0}|h[{0}]|h", bot->GetName());
        for (std::size_t i = 0; i < trip.items.size(); i += ITEMS_PER_LINE)
        {
            std::string links;
            for (std::size_t j = i; j < std::min(i + ITEMS_PER_LINE, trip.items.size()); ++j)
            {
                if (!links.empty())
                {
                    links += ", ";
                }
                links += FormatItem(trip.items[j].first, trip.items[j].second);
            }

            bool last = i + ITEMS_PER_LINE >= trip.items.size();
            lines.push_back(Acore::StringFormat("{}{} {} {}{}", PocketVendor::CHAT_PREFIX, botName,
                i == 0 ? "bought" : "also bought", links,
                last ? Acore::StringFormat(" |cffaaaaaafor|r {}", FormatMoney(trip.cost)) : std::string()));
        }

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsInWorld() || IsBot(member))
            {
                continue;
            }

            ChatHandler handler(member->GetSession());
            for (std::string const& line : lines)
            {
                handler.SendSysMessage(line);
            }
        }
    }

    void BuyBotSupplies(Player* bot)
    {
        if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight())
        {
            return;
        }

        ShoppingTrip trip;

        if (Optional<SupplyKind> ammoKind = EquippedAmmoKind(bot))
        {
            if (ItemTemplate const* ammo = BestFor(bot, *ammoKind))
            {
                TopUp(bot, ammo, sSuppliesConfig.botAmmo, trip);

                // Equip the new ammo unless the bot already shoots something at least as good
                uint32 currentId = bot->GetUInt32Value(PLAYER_AMMO_ID);
                ItemTemplate const* current = currentId ? sObjectMgr->GetItemTemplate(currentId) : nullptr;
                bool currentUsable = current && bot->GetItemCount(currentId) > 0;
                if (bot->GetItemCount(ammo->ItemId) > 0 && (!currentUsable || current->ItemLevel < ammo->ItemLevel))
                {
                    bot->SetAmmo(ammo->ItemId);
                }
            }
        }

        for (ItemTemplate const* reagent : ReagentsFor(bot))
        {
            TopUp(bot, reagent, sSuppliesConfig.botReagents, trip);
        }

        TopUp(bot, BestFor(bot, KIND_HEALING_POTION), sSuppliesConfig.botPotions, trip);
        if (UsesMana(bot))
        {
            TopUp(bot, BestFor(bot, KIND_MANA_POTION), sSuppliesConfig.botPotions, trip);
            TopUp(bot, BestFor(bot, KIND_DRINK), sSuppliesConfig.botDrink, trip);
        }
        TopUp(bot, BestFor(bot, KIND_FOOD), sSuppliesConfig.botFood, trip);

        if (sSuppliesConfig.botTellGroup)
        {
            TellGroup(bot, trip);
        }
    }

    // ------------------------------------------------------------------
    // Shop (addon messages)
    // ------------------------------------------------------------------

    void SendShop(Player* player)
    {
        SendAddon(player, Acore::StringFormat("SHOP:STATE:{}", sSuppliesConfig.shopEnabled ? 1 : 0));
        SendAddon(player, "SHOP:BEGIN");
        if (sSuppliesConfig.shopEnabled)
        {
            for (Offer const& offer : OffersFor(player))
            {
                SendAddon(player, Acore::StringFormat("SHOP:ITEM:{}:{}:{}:{}",
                    offer.category, offer.proto->ItemId, LotSize(offer.proto), LotPrice(offer.proto)));
            }
        }
        SendAddon(player, "SHOP:END");
    }

    // args is "<id>:<lots>"
    void HandleBuy(Player* player, std::string_view args)
    {
        if (!sSuppliesConfig.shopEnabled)
        {
            SendAddonError(player, "The shop is disabled on this server.");
            return;
        }

        if (player->IsInCombat())
        {
            SendAddonError(player, "You can't shop while in combat.");
            return;
        }

        std::size_t colon = args.find(':');
        Optional<uint32> itemId = Acore::StringTo<uint32>(args.substr(0, colon));
        Optional<uint32> lots = colon != std::string_view::npos ? Acore::StringTo<uint32>(args.substr(colon + 1)) : Optional<uint32>(1);
        if (!itemId || !lots || *lots == 0)
        {
            return;
        }

        // Only what the shop offers this player can be bought
        std::vector<Offer> offers = OffersFor(player);
        auto offer = std::find_if(offers.begin(), offers.end(), [&](Offer const& o) { return o.proto->ItemId == *itemId; });
        if (offer == offers.end())
        {
            SendAddonError(player, "That item isn't sold to you.");
            return;
        }

        uint32 bought = 0;
        uint32 cost = 0;
        switch (Buy(player, offer->proto, *lots, 0, bought, cost))
        {
            case BuyResult::NO_MONEY:
                SendAddonError(player, "You don't have enough money.");
                return;
            case BuyResult::NO_SPACE:
                SendAddonError(player, "Your bags are full.");
                return;
            case BuyResult::OK:
                break;
        }

        ChatHandler(player->GetSession()).SendSysMessage(Acore::StringFormat("{}Bought {} for {}",
            PocketVendor::CHAT_PREFIX, FormatItem(offer->proto, bought), FormatMoney(cost)));
        SendAddon(player, Acore::StringFormat("BOUGHT:{}:{}:{}", offer->proto->ItemId, bought, cost));
    }
}

bool PocketVendor::HandleSupplyMessage(Player* player, std::string_view body)
{
    if (body == "SHOP")
    {
        SendShop(player);
        return true;
    }

    if (body.rfind("BUY:", 0) == 0)
    {
        HandleBuy(player, body.substr(4));
        return true;
    }

    return false;
}

class PocketVendorSuppliesPlayer : public PlayerScript
{
public:
    PocketVendorSuppliesPlayer() : PlayerScript("PocketVendorSuppliesPlayer", { PLAYERHOOK_ON_UPDATE }) {}

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        BotSupplyTimer* timer = player->CustomData.Get<BotSupplyTimer>(BOT_TIMER_KEY);
        if (!timer)
        {
            // First update of this character: remember whether it is a bot
            timer = player->CustomData.GetDefault<BotSupplyTimer>(BOT_TIMER_KEY);
            timer->isBot = IsBot(player);
            // Spread the first shopping trip so bots don't all buy on the same tick
            timer->timer = int32(urand(10000, std::max<uint32>(sSuppliesConfig.botInterval, 10000)));
        }

        if (!timer->isBot)
        {
            return;
        }

        timer->timer -= int32(diff);
        if (timer->timer > 0)
        {
            return;
        }

        timer->timer = int32(std::max<uint32>(sSuppliesConfig.botInterval, 10000));
        if (sSuppliesConfig.botsEnabled && player->IsInWorld())
        {
            BuyBotSupplies(player);
        }
    }
};

class PocketVendorSuppliesWorld : public WorldScript
{
public:
    PocketVendorSuppliesWorld() : WorldScript("PocketVendorSuppliesWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sSuppliesConfig.shopEnabled = sConfigMgr->GetOption<bool>("PocketVendor.Shop.Enable", true);
        sSuppliesConfig.priceRate = sConfigMgr->GetOption<uint32>("PocketVendor.Shop.PriceRate", 100);

        sSuppliesConfig.botsEnabled = sConfigMgr->GetOption<bool>("PocketVendor.BotSupplies.Enable", true);
        sSuppliesConfig.botInterval = sConfigMgr->GetOption<uint32>("PocketVendor.BotSupplies.Interval", 300000);
        sSuppliesConfig.botGoldReserve = sConfigMgr->GetOption<uint32>("PocketVendor.BotSupplies.GoldReserve", 10000);
        sSuppliesConfig.botAmmo = sConfigMgr->GetOption<uint32>("PocketVendor.BotSupplies.Ammo", 1000);
        sSuppliesConfig.botReagents = sConfigMgr->GetOption<uint32>("PocketVendor.BotSupplies.Reagents", 20);
        sSuppliesConfig.botPotions = sConfigMgr->GetOption<uint32>("PocketVendor.BotSupplies.Potions", 5);
        sSuppliesConfig.botFood = sConfigMgr->GetOption<uint32>("PocketVendor.BotSupplies.Food", 0);
        sSuppliesConfig.botDrink = sConfigMgr->GetOption<uint32>("PocketVendor.BotSupplies.Drink", 0);
        sSuppliesConfig.botLog = sConfigMgr->GetOption<bool>("PocketVendor.BotSupplies.Log", false);
        sSuppliesConfig.botTellGroup = sConfigMgr->GetOption<bool>("PocketVendor.BotSupplies.TellGroup", true);
        sSuppliesConfig.botRefillPercent = std::min<uint32>(sConfigMgr->GetOption<uint32>("PocketVendor.BotSupplies.RefillPercent", 25), 100);
    }

    void OnStartup() override
    {
        BuildCatalog();
        LOG_INFO("module", "Pocket Vendor: supply catalog has {} arrows, {} bullets, {} healing and {} mana potions, {} food and {} drinks.",
            sCatalog[KIND_ARROW].size(), sCatalog[KIND_BULLET].size(), sCatalog[KIND_HEALING_POTION].size(),
            sCatalog[KIND_MANA_POTION].size(), sCatalog[KIND_FOOD].size(), sCatalog[KIND_DRINK].size());
    }
};

void AddPocketVendorSuppliesScripts()
{
    new PocketVendorSuppliesPlayer();
    new PocketVendorSuppliesWorld();
}
