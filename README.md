# AnimationWatch

Shows which animation the player is really playing, after Open Animation Replacer / DAR picked the replacement, and catches animation packs that fight over the same animation (the cause of stutters, limps and runs cut off mid-step), and shows per activity and gear which pack wins each animation, with a one-click way to pick the winner. Skyrim SE 1.5.97 and AE (SKSE).

Targets: Skyrim SE 1.5.97 and AE (Address Library).
Requires: SKSE, Address Library for SKSE Plugins. Optional: SKSE Menu Framework (F1 menu).

What it does
------------
Logs every animation clip the player's behavior graphs start (third person and first person),
after Open Animation Replacer / DAR picked their replacement, plus loop restarts (and whether a
loop switched to another variant file) and paired animations (kill moves etc.).
Optionally shows each new animation in the notification area, e.g.
  Anim: inj2.hkx  [OAR: EVGCI / Player Injured]

Flip-flop warning (1.1.0): when the same animation slot (for example DefaultMale MT_RunForward)
switches to a different mod's file more than twice within 2 seconds, a WARNING line goes into the
session log and a notification is shown, e.g.
  Anim flip-flop: MT_RunForward switched 3 times in 1.2 s: OAR: CATA / Light Armor <-> OAR: CLAMBER / Upwards Run
That pattern restarts the animation mid-step (stutter / limp). Each slot warns at most every 10 s.

1.4.0 - Animation dolls (F1 > Animation Watch > Animation Dolls)
----------------------------------------
A page of pictures, one per activity (Running, Sprinting, Walking, Sneaking, Jumping, Swimming,
One-handed, Two-handed, Bow, Spellcasting, Unarmed, Horse, Idle, Other). Click a picture to see it
large with every animation of that activity around it, each pointing at the picture (left-side
animations on the left, right-side on the right, weapon animations at the weapon) and naming the
mod it comes from (the Mod Organizer mod, and the OAR pack / submod or DAR folder).
"Back to all pictures" returns. Labels are coloured:
  green  = one pack replaces it
  yellow = two or more packs compete (highest OAR priority wins when its conditions pass)
  red    = it flip-flopped between packs this session
  grey   = the game's own animation
"Gear" switches the view: All gear / Light armor / Heavy armor / Clothing (packs whose conditions
use the ArmorLight / ArmorHeavy / ArmorClothing keywords, or have those words in their folder
names). "First person" shows first-person animations.
Click a label to see the competing packs:
  Make winner        - raises that submod's OAR priority just above the others for that animation
  Make whole mod win - raises every submod of that mod by the same amount (their order is kept)
  Undo               - puts the original priority back
The change is written to the submod's user.json (OAR's own override file); the original priority
is kept in it as "animationWatchOriginalPriority", and an existing user.json is backed up to
user.json.animationwatch.bak. Restart the game for OAR to use the new priority.
Old DAR folders: "Rename it for me" renames the folder to the first free number above the others
(after "Yes, rename it"; under MO2 the real folder inside the mod is renamed). Restart the game.

F1 menu, section "Animation Watch":
  - Show animation names on screen (on/off)
  - Write session log file (on/off)
  - Show flip-flop warnings on screen (on/off; warnings still go to the log while logging is on)
  - Number of flip-flop warnings this session
  - Last animation
Settings are saved to Data\SKSE\Plugins\AnimationWatch.ini (MO2 puts the saved copy in overwrite).

Files
-----
Session log: Documents\My Games\Skyrim Special Edition\SKSE\AnimationWatch\Session_<date>_<time>.log
  columns: time | graph (DefaultMale/DefaultFemale/FirstPerson) | event | clip | animation file
Plugin log:  Documents\My Games\Skyrim Special Edition\SKSE\AnimationWatch.log

Output: build\Release\AnimationWatch.dll and .pdb

Build (Developer Command Prompt for VS 2022), one line at a time:
  cd /d C:\dllmaker\AnimationWatch
  cmake --preset vs2022-windows
  cmake --build build --config Release

Install (its own MO2 mod):
  SKSE\Plugins\AnimationWatch.dll
  SKSE\Plugins\AnimationWatch.pdb

1.2.0 - Fixer catalog
---------------------
When a flip-flop warning fires, every OAR submod involved whose settings say "interruptible": true is
listed in F1 > Animation Watch > Fixes, with Apply and Undo buttons.

Fixer #1 (flip-flop: interruptible off)
  Apply: writes user.json in that submod's folder (OAR's own per-user settings file): a copy of the
         submod's settings with "interruptible" set to false. MO2 puts the file in overwrite.
  Undo:  removes that user.json (or restores the user.json that was there before, saved as
         user.json.animationwatch.bak).
  Takes effect after the game restarts (OAR reads user.json at startup).
  Submods that are already not interruptible are not offered: their switches come from fresh
  animation starts, not mid-step cuts, so this fixer would change nothing.
  Every suggestion, apply and undo is written to the session log (FIX SUGGESTED / FIX APPLIED / FIX UNDONE).
  A new suggestion also pops up the game's message box (stays until OK), once per submod per session.

1.2.2
-----
Skyrim SE 1.5.97 support: the notification call now uses the SE address too (SE 52050 / AE 52933).
Everything else already used addresses for both.

## How AI was used

AnimationWatch was made by tonycubed2 working together with Anthropic's Claude (an AI assistant). tonycubed2
decided what the mod should do, set the requirements, built every DLL on his own PC and tested it in his own
heavily modded game. Claude wrote the C++ source code to those requirements, checked how the game and Open
Animation Replacer work against their own code, and fixed the build errors tonycubed2 reported back.

## License

Copyright (C) 2026 tonycubed2

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
[LICENSE](LICENSE) (the GNU General Public License v3) for details.

AnimationWatch links [CommonLibSSE NG](https://github.com/alandtse/CommonLibVR/tree/ng), licensed
GPL-3.0-or-later WITH the Modding Exception AND the GPL-3.0 Linking Exception (with Corresponding Source).
Because it links CommonLibSSE NG, AnimationWatch is released under the GPL-3.0-or-later, and its complete
source code is in this repository. `src/SKSEMenuFramework.h` is the SKSE Menu Framework API header (MIT,
see `src/SKSEMenuFramework_LICENSE.txt`), included unchanged so the source builds as is.
