# AnimationWatch

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

Animation dolls (1.3.0, pictures 1.4.0): F1 > Animation Watch > Animation Dolls.
A page of pictures, one per activity (Running, Sprinting, Walking, Sneaking, Jumping, Swimming,
One-handed, Two-handed, Bow, Spellcasting, Unarmed, Horse, Idle, Other). Click a picture to see it
large with every animation of that activity around it, each pointing at the picture (left-side
animations on the left, right-side on the right, weapon animations at the weapon) and naming the
pack it comes from. "Back to all pictures" returns. Labels are coloured:
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

One build folder only: build
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

1.8.7
-----
Viewing room: the clone was invisible (and its gear missing, and the camera stuck in third person)
because the world was stood still the moment the clone was placed, before its fade-in, its gear
and the camera switch (all on game time) had finished. The world now only stands still once the
clone is dressed, fully faded in and the camera is first person in the room (or after 5 s, with
a log line saying which part was missing). The clone is forced fully visible when its 3D loads,
the camera switch is retried every 0.5 s until it takes, the gear is checked (and the model
rebuilt if it did not attach), and the player's own third-person body is hidden in the room and
shown again on leaving. The SKSE log has a "Clone: gate:" line for each step.
Bigger buttons: "Back to all pictures" and "Leave the viewing room" (F1 page and the room's
control panel) are now large orange buttons on their own line.

1.8.8
-----
Free camera in the viewing room: watch the clone's animation from any angle, also while the world
stands still. Turn it on with C in the room, the blue "Free camera" button in the room's control
panel, or the "Free camera" button on the F1 Animation Dolls page (while you are in the room).
  Mouse          look around (the room's control panel closes while the free camera is on, so
                 the mouse turns the camera; "Invert mouse Y" is in the panel)
  W / S          forward / back (where you look)
  A / D          left / right
  E / Q          up / down
  Shift (held)   4x faster
  Wheel, + / -   slower / faster (20 to 3000 units a second, starts at 200)
  C              free camera off again (back to the first-person view; the panel comes back)
  R or "Reset view"  free camera off, first person, facing the clone
The room's own keys still work while it is on (none of them is shared): Space pause/play,
Left/Right frame, Up/Down playback speed, L loop, Backspace restart, F1 pick another, Esc leave.
The auto-move that C starts in the game is cancelled in the room.
The free camera turns off by itself when you leave the room, the clone is removed, or a game is
loaded; while F1 or another menu is open its keys and mouse are ignored.
How: it uses the game's own free camera (as the console's tfc), but moves it itself with real
time, because the game's own free camera moves on game time, which stands still in the room.
Log: every line is now written to AnimationWatch.log at once (no need to quit the game first).
"Free camera:" lines log on / off, the position now and then, and any error.

1.8.9
-----
Free camera twice as fast: 400 units a second to start (40 to 6000).
On-screen camera controls: while the free camera or the orbit view is on, a small panel at the
bottom of the screen has big "<< Slower" / "Faster >>" buttons and a camera speed slider (each
button press: 1.5x), plus "Orbit view (O)" / "Stop orbit", "Free camera off (C)", "Reset view (R)",
"Hide controls" and "Invert mouse Y". The mouse wheel and + / - still change the speed too.
With the panel shown the mouse is a cursor for it: hold the RIGHT mouse button to look around.
"Hide controls" goes back to the 1.8.8 way (the mouse always looks); a right click shows them again.
Orbit view: the camera circles the clone by itself (20 s a circle to start, at the distance the
camera was, slightly above the chest, always looking at the clone), also while the world stands
still. Start it with O in the room, the blue "Orbit view (O)" button in the room's panel, on the
F1 page (while in the room) or on the camera panel. While it runs the camera panel also has
"<< Orbit slower" / "Orbit faster >>", a seconds-a-circle slider, and Distance and Height sliders.
Any keyboard key or a right click stops it (left clicks are for the panel; wheel and + / - only
change the speed). Note the key you press also does its own job (Space still pauses, Esc still
leaves the room). Started from the normal view it goes back to the normal first-person view;
started from the free camera ("Orbit view" on the camera panel) it goes back to flying from
where it stopped; "Fly from here" on the panel does that too. It turns off with the free camera
(leaving the room, removing the clone, loading a game).

1.9.0
-----
Competing versions in the viewing room: when the animation the clone plays is replaced by two or
more animation packs, a RED dropdown "Competing versions (N) of <animation>" shows in the room's
control panel, on the camera panel (free camera / orbit view) and on the F1 Animation Dolls page
while you are in the room. It lists every pack's version (same names as the doll labels:
"Mod: OAR mod / submod" or "Mod (DAR folder N)"), highest priority first; the one that wins is
marked "(winner)" and is selected when you first play the animation. Pick another and the clone
plays that pack's own file at once, so you can try them one by one; pick the winner to go back.
Pause, speed, loop and frame steps keep working. This is only a preview: nothing on disk changes.
"Make this the winner" next to it does what "Make winner" does on the page (raises that OAR
submod's priority; restart the game for OAR to use it). Old DAR folders are renamed from the page.
Only packs that apply to the gear filter on the Dolls page are listed. A version limited by its
pack's conditions is played anyway in the preview (in the game it only plays when they pass).
Every pick is written to the log ("Viewing room: competing version picked ...").

1.9.1
-----
Fix: switching off "Show flip-flop warnings on screen" in the F1 menu did not stop the
"AnimationWatch - FIX AVAILABLE" pop-up box that comes with a flip-flop. Now the setting (renamed
"Show flip-flop warnings and fix pop-ups on screen") stops both. Flip-flops are still written to
the session log, still marked red on the Animation Dolls page, and their fixes are still listed
in F1 > Animation Watch > Fixes. The SKSE log now reports the right plugin version (it said 1.8.6).

1.9.2
-----
Animation Dolls: new "Kill moves" tile (its own picture) for kill moves and other paired animations;
they were on the Other tile before. Attacks done while running, sprinting, walking or sneaking now
show on their weapon's tile (One-handed, Two-handed, Bow, Unarmed, Spellcasting); before, a running
or sprinting power attack landed on the Running or Sprinting tile and never showed with its weapon.
