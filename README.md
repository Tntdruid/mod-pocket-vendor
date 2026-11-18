# Pocket Vendor

An AzerothCore module that automatically sells junk when a player loots it, plus a small addon for personal sell and keep lists.

<img width="501" height="607" alt="{749D2174-6965-49A7-B35E-B8E3239F3BD0}" src="https://github.com/user-attachments/assets/ac974610-bc4a-44f0-8a7d-1900d5b11e29" />

## What it does

- **Sells grey items on loot:** a looted grey item is sold right away, and the money goes straight to the player.
- **Personal lists (with the addon):** items on your sell list are sold on loot whatever their quality. Items on your keep list are never sold, not even grey ones.
- **Optionally sells white weapons and armor too.** Trade goods, shirts, tabards and fishing poles are never sold.
- **Reports sales in chat:** either one line per item, or one summary line after looting. Both use vendor-style coin icons.

## Never sold

- Quest items, items that start a quest, and items one of the player's active quests needs
- Items with no vendor value (they stay in the bags)
- Items on the server's blacklist
- Items on the player's keep list
- Anything while the player is at the gold cap

## The PocketVendor addon

The addon is in `addon/PocketVendor`. Copy that folder to `World of Warcraft/Interface/AddOns/`.

- Open it with `/pv`, or with the **Pocket Vendor** button on any merchant window.
- **Sell** and **Keep** tabs. Drag an item onto the window, or shift-click an item into the box and press **Add**.
- Right-click an item in the list to remove it.
- The **Auto-sell on loot** checkbox does the same as `.vendor on/off`.
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

Run `.reload config` to apply changes without restarting the server.

## Changelog

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
