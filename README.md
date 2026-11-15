# Space Destroyer 3D for Nintendo DS / DS Lite

DS port of *Space Destroyers 3D* by AeEn123 (Scratch), based on the [3DS/PC port](https://github.com/AeEn123AI/space-destroyers-3d-port).

**No assets or prebuilt ROMs are included**: the music is Sega's (SMS BIOS / Space Harrier), so it can't be
redistributed here. Build it yourself from the original Scratch project, see Building below.

Copy `dist/space_destroyer.nds` to your flashcart or SD card and start it from your kernel or TWiLight Menu++. It's a single
file: music is embedded in the game's code, so it needs no argv, NitroFS or card access. Records are saved to `/data/space-destroyer/records.txt`
on the card if the loader gives the game card access (needs a working DLDI patch); the title screen says whether they really save.

`dist/space_destroyer_sdde.nds` is the same game with the game code older builds used, `SDDE` instead of the homebrew `####`. Use it only if the
main file won't boot on your loader.

Closing the lid sleeps the DS: screens and sound off, a running game comes back paused. melonDS 1.1 doesn't wake from this.

## Controls
| Input | Action |
| --- | --- |
| D-pad | Move (menus: choose) |
| A / B | Shoot (menus: A selects, B goes back) |
| L / R | Slower / faster |
| Start | Pause (title: play) |
| Select | Quit to title |
| X | Music on/off |
| Touch: drag | Steer |
| Touch: tap | Shoot |

The top screen shows the game, the bottom screen has the HUD and touch menu.

## Building
Needs devkitPro (`dkp-pacman -S nds-dev`).

    export DEVKITPRO=/opt/devkitpro
    export DEVKITARM=$DEVKITPRO/devkitARM PATH=$DEVKITPRO/tools/bin:$PATH
    # 1. in a checkout of the 3DS/PC port next to this one, put the Scratch project in as `Space Destroyers 3D.sb3`
    #    and run `make assets` (see that repo's README)
    # 2. generate this port's gen/ folder from those assets:
    python3 tools/build_ds_assets.py ../space-destroyers-3d-port
    make dist

Fonts baked into the ROM: Liberation Sans, DejaVu Sans, Grand9K Pixel. Their licences are in the 3DS/PC port's [`tools/fonts`](https://github.com/AeEn123AI/space-destroyers-3d-port/tree/main/tools/fonts).
