# Pocket Vendor

An AzerothCore module that automatically sells junk when a player loots it, lets bots buy their own supplies, and comes with a small addon for personal sell and keep lists and a supply shop.

<img width="501" height="607" alt="{749D2174-6965-49A7-B35E-B8E3239F3BD0}" src="https://github.com/user-attachments/assets/ac974610-bc4a-44f0-8a7d-1900d5b11e29" />

## What it does

- **Sells grey items on loot:** a looted grey item is sold right away, and the money goes straight to the player.
- **Personal lists (with the addon):** items on your sell list are sold on loot whatever their quality. Items on your keep list are never sold, not even grey ones.
- **Optionally sells white weapons and armor too.** Trade goods, shirts, tabards and fishing poles are never sold.
- **Reports sales in chat:** either one line per item, or one summary line after looting. Both use vendor-style coin icons.
- **Bots buy their own supplies (mod-playerbots):** every 5 minutes, wherever they are, bots top up ammo, reagents for the spells they know, and healing and mana potions. They pay the vendor price from their own gold.
- **Supply shop (with the addon):** players can buy the same things from the addon's Shop tab, anywhere, at the vendor price.

## Supplies

Nothing is free. Every purchase costs the item's vendor price (times `PocketVendor.Shop.PriceRate`), paid from the character's own gold. Each character is offered the best item of each kind for their level:

| Kind          | Where it comes from                                                                 | Who gets it                                      |
|---------------|-------------------------------------------------------------------------------------|--------------------------------------------------|
| Arrows        | Ammo sold for gold by any vendor                                                    | Bow or crossbow users (bots: the equipped weapon) |
| Bullets       | Ammo sold for gold by any vendor                                                    | Gun users (bots: the equipped weapon)            |
| Reagents      | Vendor-sold reagents of spells the character knows (Symbol of Kings, Arcane Powder, Wild Quillvine, Ankh, ...) | Everyone with such spells |
| Potions       | Healing and mana potions made by Alchemy, since no vendor sells potions            | Everyone; mana potions only for mana users       |
| Food & drink  | Food and drink sold for gold by any vendor                                          | Shop: everyone. Bots: off by default             |

Bots also equip the new ammo when it is better than what they shoot. A bot in your party or raid tells you in chat what it bought. They never spend below `PocketVendor.BotSupplies.GoldReserve`.

To stop bots getting the same things for free, set `Playerbots.AltMaintenanceAmmo`, `AltMaintenanceReagents`, `AltMaintenancePotions` and `AltMaintenanceFood` to `0` in `playerbots.conf`. This module replaces mod-bot-reagents, so remove that module, or bots shop twice.

## Never sold

- Items that start a quest, and items one of the player's active quests needs
- Quest-class items such as Green Hills of Stranglethorn pages, unless the player puts them on their sell list
- Items with no vendor value (they stay in the bags)
- Items on the server's blacklist
- Items on the player's keep list
- Anything while the player is at the gold cap

## The PocketVendor addon

The addon is in `addon/PocketVendor`. Copy that folder to `World of Warcraft/Interface/AddOns/`.

- Open it with `/pv`, or with the **Pocket Vendor** button on any merchant window.
- **Sell** and **Keep** tabs. Drag an item onto the window, or shift-click an item into the box and press **Add**.
- For a ranked item such as Scroll of Spirit IV, you are asked whether to add every rank or only that one.
- Right-click an item in the list to remove it.
- The **Auto-sell on loot** checkbox does the same as `.vendor on/off`.
- **Shop** tab: the supplies for your level with their price. **Buy** buys one lot (e.g. 200 arrows); shift-click buys a full stack. You can't shop in combat.
- `/pv sync` reloads the lists from the server.

The lists are stored on the server per character, so they follow you to any computer. An item can only be on one list at a time; adding it to the other list moves it.

## Player commands

| Command       | Effect                                   |
|---------------|------------------------------------------|
| `.vendor`     | Show whether auto-selling is on or off   |
| `.vendor on`  | Turn auto-selling on for this character  |
| `.vendor off` | Turn auto-selling off for this character |

The on/off choice is saved across logins only when `EnablePlayerSettings = 1` is set in `worldserver.conf`.

## Installation

1. Clone the module into your server's `modules` folder, then re-run CMake and rebuild:

   ```bash
   cd azerothcore-wotlk/modules
   git clone https://github.com/Tntdruid/mod-pocket-vendor.git
   ```

   To update later, run `git pull` in `modules/mod-pocket-vendor` and rebuild.
2. The `mod_pocket_vendor_list` table in the characters database is created automatically at startup from `data/sql/db-characters`. If you have turned automatic updates off, run that file by hand.
3. Copy `conf/mod_pocket_vendor.conf.dist` to `mod_pocket_vendor.conf` in your config's `modules` folder.

## Configuration

| Option                        | Default | Description                                         |
|-------------------------------|---------|-----------------------------------------------------|
| `PocketVendor.Enable`         | `1`     | Enable the module                                   |
| `PocketVendor.Announce`       | `2`     | `0` no message, `1` one line per item, `2` summary  |
| `PocketVendor.SummaryDelay`   | `1000`  | Milliseconds to wait before sending the summary     |
| `PocketVendor.SellCommonGear` | `0`     | Also sell white weapons and armor                   |
| `PocketVendor.Blacklist`      | `""`    | Comma-separated item IDs that are never sold        |
| `PocketVendor.PlayerToggle`   | `1`     | Allow `.vendor on/off`                              |
| `PocketVendor.Playerbots`     | `1`     | Let mod-playerbots bots auto-sell too               |
| `PocketVendor.PlayerLists`    | `1`     | Allow personal sell/keep lists from the addon       |
| `PocketVendor.MaxListSize`    | `200`   | Max items per character across both lists           |
| `PocketVendor.Shop.Enable`    | `1`     | Players can buy supplies from the Shop tab          |
| `PocketVendor.Shop.PriceRate` | `100`   | Price in percent of the vendor price                |
| `PocketVendor.BotSupplies.Enable` | `1` | Bots buy their own supplies                         |
| `PocketVendor.BotSupplies.Interval` | `300000` | Milliseconds between a bot's shopping trips  |
| `PocketVendor.BotSupplies.GoldReserve` | `10000` | Copper a bot never spends                  |
| `PocketVendor.BotSupplies.Ammo` | `1000` | Ammo a bot keeps                                  |
| `PocketVendor.BotSupplies.Reagents` | `20` | Of each reagent a bot keeps                     |
| `PocketVendor.BotSupplies.Potions` | `5` | Of each potion a bot keeps                        |
| `PocketVendor.BotSupplies.Food` | `0`   | Food a bot keeps (`0` = off)                        |
| `PocketVendor.BotSupplies.Drink` | `0`  | Drink a bot keeps (`0` = off)                       |
| `PocketVendor.BotSupplies.Log` | `0`    | Log every bot purchase to the worldserver log       |
| `PocketVendor.BotSupplies.TellGroup` | `1` | Bots tell your party/raid in chat what they bought |

Run `.reload config` to apply changes without restarting the server.

## Changelog

### v1.2.0

- Added bot shopping: bots buy their own ammo, reagents and potions every 5 minutes, wherever they are, and pay the vendor price from their own gold. They equip better ammo. Replaces mod-bot-reagents. Bots in your party or raid tell you in chat what they bought. `PocketVendor.BotSupplies.Log = 1` logs each purchase.
- Added a Shop tab to the addon where players buy ammo, reagents, potions, food and drink for their level, anywhere, at the vendor price.

### v1.1.0

- Added a question when you add a ranked item such as Scroll of Spirit IV to the Sell or Keep list: add every rank, or only that one. Ranks that can't be sold are skipped.
- Fixed quest-class items such as Green Hills of Stranglethorn pages not being allowed on the sell list. They are still kept while an active quest needs them.

### v1.0.0

- Initial release
- Sells grey items on loot, with quest items, worthless items and a server blacklist protected
- Optional selling of white weapons and armor
- Chat report per item or as one summary line, with coin icons
- `.vendor on/off` per character
- PocketVendor addon with personal sell and keep lists stored on the server
- Playerbots support

## License

MIT. See [LICENSE](LICENSE).
