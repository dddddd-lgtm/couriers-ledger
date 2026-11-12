# The Courier's Ledger

A ledger of everyone you have met in Skyrim: where they are now, what they teach, what they sell, and a
quest marker on the one you are looking for. It is a helper, not a cheat. Nobody is teleported and
nothing can be taken.

## What it does

- **Everyone you have talked to.** The game already remembers who has spoken with you (the condition
  GetTalkedToPC), so the ledger fills itself, also in an existing save.
- **Quest targets.** People that a running quest points at are listed too, in yellow, with the quest's
  name, even before you have met them.
- **Find a name fast.** Alphabetical list with tabs (All, A-F, G-L, M-R, S-Z) and a name search.
- **Colour by role.** Yellow quest target, blue trainer, gold merchant, purple fence, orange innkeeper,
  green follower, pink can marry, grey dead. Each role has its own filter.
- **Details.** Where the person is now, what and how high they train, what they trade in and when, how
  they feel about you, and how far away they are when they are near.
- **Inventory and stock, look only.** A person's inventory and a merchant's stock, with the effects of
  enchanted items, potions and scrolls (for example Fortify Alteration 12). Ingredients are shown
  without effects: those are for you to discover.
- **Find Item.** Search by name or by effect ("robe", "alteration", "fortify smithing") in the stock of
  merchants you know and, if you like, in people's inventories. Every hit can be tracked.
- **Track on map.** One click gives the person a quest marker on the map, the compass and above their
  head, also through walls and floors. While you track someone, the other quests' markers are hidden so
  yours stands out, and they come back when you stop tracking.

## Requirements

- SKSE64
- Address Library for SKSE Plugins
- SKSE Menu Framework 3

Tested on Skyrim SE 1.6.1170. Built with CommonLibSSE-NG for 1.5.97 and 1.6, but only 1.6.1170 has been
tested in game. VR is not supported (SKSE Menu Framework does not support VR).

## Use

Open the SKSE Menu Framework menu (its hotkey is set in SKSE Menu Framework's own ini) and pick
**The Courier's Ledger**. There are two pages: **People** and **Find Item**.

The plugin is light (ESPFE) and safe to add mid-game. To remove it, press **Stop tracking** first if you
are tracking someone, so that the other quests' markers come back.

## Building

- `plugin/`: CouriersLedger.dll, C++23, CommonLibSSE-NG 13.1.0, built with xmake.
  `include/SKSEMenuFramework.h` comes from QTR-Modding/SKSE-Menu-Framework-3-Example (MIT, licence next
  to it).
- `esp/`: writes CouriersLedger.esp with Mutagen: a light plugin with one quest (CL_TrackQuest, FormID
  0x800) that has one reference alias and one objective.
- `.github/workflows/build.yml` builds both on GitHub Actions and packs the release zip. It fetches
  CommonLibSSE-NG into `lib/commonlibsse-ng` at a fixed commit. To build locally on Windows, clone
  CommonLibSSE-NG there with its submodules, then run `xmake` in `plugin/`.
- `docs/nexus-description.bbcode` is the text of the Nexus mod page.

## Credits

- Made by dqann and Claude.
- The SKSE team, the CommonLibSSE-NG contributors, and the Address Library author.
- QTR-Modding for SKSE Menu Framework 3.
- The Mutagen team.

## Licence

MIT, see LICENSE.
