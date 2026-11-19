# Legends of Might and Magic Comprehensive Modern Patch

This project is a set of patches for Legends of Might and Magic (New World Computing / 3DO, 2001, LithTech 2) that
makes the game work well on modern Windows. The patches are applied in memory on startup; no game file is modified.
The [LANCommander Interposer](https://github.com/LANCommander/LANCommander.Interposer) is built into the patch. The
following fixes/improvements are included:

- **No startup hang or crash with many input devices.** Modern Windows' DirectInput 7 overruns its device list while
  it enumerates HID devices. The built-in Interposer bridges the game to DirectInput 8 instead
  (`DirectInput.FixLegacyDeviceEnumeration`). The engine is shown DirectInput 7 device types, so it still finds the
  keyboard and mouse for its key bindings.
- **Fullscreen in place, or borderless.** The fullscreen window covers the monitor from the start (it used to sit
  in the middle of the screen until an alt-tab). `Borderless 1` runs the game in a borderless window over the
  monitor instead.
- **Widescreen.** Hor+ field of view: the vertical view a 4:3 screen has, wider to the sides (90° → 106° at 16:9).
- **Scaled menus.** The menus, drawn 1:1 in a 640×480 box until now, are scaled up to fill the screen with their
  aspect ratio kept, mouse included.
- **Scaled HUD.** The HUD scales with the screen (or to a size of your choice) and stays on the screen's edges.
- **Smooth menus and console.** The menus' text, filled boxes and list backgrounds are drawn on the GPU (each used to
  lock and read back the whole screen: Video Options ran at ~5 fps), the menus no longer copy every changed rectangle
  to the screen separately, the console
  no longer locks the screen once per line of text, and the frame limiter no longer oversleeps (1 ms timer, precise
  MaxFPS).
- **Relocated saves.** `autoexec.cfg` (settings and key bindings), favourites, buddies, the map rotation, screenshots
  and the game's registry settings go to `%USERPROFILE%\Saved Games\New World Computing\Legends of Might and Magic`.
  Existing files are copied over the first time (and moved from `%USERPROFILE%\New World Computing\...`, where patch
  1.0.0 kept them).
- **Voice chat.** Proximity voice and a team radio (team games) through the Interposer's voice chat. The game server
  (a hosting game or the dedicated server) runs the voice server, mirrors its players, teams and positions into it,
  and advertises its port in its GameSpy query replies (`\commsport\`), so clients find it on their own.
- **FastDL.** A server can advertise a FastDL server in its query replies (`\fastdlport\`, `\fastdlhost\`,
  `\fastdlpath\`); joining it, clients download the custom worlds they lack.
- **Rich presence.** What you are doing shows in Discord and in the LANCommander launcher: the menus, or the world,
  game type, server name, player count and time in the world.

It only supports `lithtech.exe` of version 1.1 (PE timestamp `0x31313931`) with its `cshell.dll` / `object.lto`, and
`LegendServ.exe` 1.0 (the dedicated server). Anything else is left alone and logged.

> **Status:** written from static analysis of the game; every hook checks the original bytes before it patches and
> logs what it did to `lommpatch.log`. Report anything the log says was "left alone".

## Install

Download one of the packages from the releases and extract it into the folder that holds `lithtech.exe`.

| package | contents | for |
|---|---|---|
| `lommpatch-<version>-dinput.zip` | `dinput.dll`, `lommpatch.ini`, `lommpatch\` | a plain install: the game loads `dinput.dll` from its folder (replaces DxWrapper's `dinput.dll` converter, which is not needed) |
| `lommpatch-<version>-asi.zip` | `lommpatch.asi`, `lommpatch.ini`, `lommpatch\` | an ASI loader that is already installed (Ultimate ASI Loader); also the way into the dedicated server `LegendServ.exe` |
| `LANCommander.Interposer.Plugin.LegendsOfMightAndMagic-<version>.zip` | `.interposer\Plugins\LANCommander.Interposer.Plugin.LegendsOfMightAndMagic.dll`, `lommpatch\` | a game the [LANCommander Interposer](https://lancommander.app/Interposer) is deployed to |

Start the game with `lithtech.exe` (the launcher, `legends.exe`, is not needed).

With the plugin build the DirectInput fix and FastDL are the Interposer's own settings: set
`DirectInput.FixLegacyDeviceEnumeration: true`, and for FastDL `FastDL.Enabled: true`,
`UseDownloadDirectory: false` and a path `Local: '%GAMEDIR%\Data\Worlds'`, `Remote: Worlds` in its `Config.yml`.

### Voice chat

- The server needs the patch: hosting from the game works as is; for `LegendServ.exe` use the ASI build with an ASI
  loader deployed as `version.dll`, or the plugin build with the Interposer. Open UDP port 27800 (`VoicePort`)
  next to the game port (29800) and the query port (29801).
- Hold **Caps Lock** to talk to the players nearby and **X** to talk to your team. Both are controls in Options >
  Controls ("Voice: talk nearby", "Voice: team radio"). Options > Sound > Voice chat has the voice volume, the
  microphone's volume, the mode (push to talk or voice activated) and the microphone. Who is talking shows on the
  left of the HUD.
- Voices carry 50 m and are at full volume within 4 m (`[Server] VoiceRange`, `VoiceFullVolumeRange`, in metres; LithTech's tool metric of 52.49 world units per metre converts them).

### FastDL

On the server, serve the folder holding `Worlds\<name>.dat` (and `Worlds\<name>Files\`) over HTTP and set
`[Server] FastDLPort` (and `FastDLHost` / `FastDLPath` if it is not this server's address at `/`). Clients download the
worlds they lack into `Data\Worlds` as they join.

### Rich presence

The Interposer shows the presence in Discord when it has a Discord application's id, and always sends it to the
LANCommander launcher. With the dinput.dll and ASI builds, set `[RichPresence] DiscordApplicationId` (and
`LargeImage`, an asset key of that application or an `https://` URL); the patch adds a `RichPresence` section to
`.interposer\Config.yml` if it has none. With the plugin build, set `RichPresence.Discord.Enabled: true` and
`ApplicationId` in the Interposer's `Config.yml`. In a game the patch reads the server's GameSpy `\status\` reply every
15 s (world, game type, server name, players): this works for a server you joined and for one you host.

## Settings

Your settings are console variables in `autoexec.cfg` in the save folder, next to the game's own (`"HorPlus" "1"`).
Change them there, in the console (`VoiceVolume 50`), or in the game's menus where they have a place. The first time
the patch runs, it adds its settings to `autoexec.cfg` with their defaults. The defaults come from
[`dist/lommpatch.ini`](dist/lommpatch.ini), which describes every option; for the Interposer plugin build they come
from `Plugins.LegendsOfMightAndMagic.<Section>.<Key>` in `.interposer\Config.yml`. `[General]` and `[Saves]` are
read from `lommpatch.ini` only, and the dedicated server reads only `lommpatch.ini`. Set a switch to `0` to turn
that fix off. Patch 1.0.0's `Settings.ini` is moved into `autoexec.cfg` once.

The game's own settings, which it used to keep in the registry, are console variables in `autoexec.cfg` too (marked
*game* below; `[Saves] GameSettings`).

| variable (`lommpatch.ini` key) | what it does |
|---|---|
| `[General] Enabled` | The patch on or off |
| `[Saves] Redirect`, `Folder` | Saves in `%USERPROFILE%\Saved Games\New World Computing\Legends of Might and Magic` (or `Folder`) |
| `[Saves] GameSettings` | The game's registry settings in `autoexec.cfg` (copied over the first time) |
| `HorPlus` | Hor+ field of view |
| `ScaleMenus`, `ScaleHUD`, `HUDScale`, `IntegerScale` | Menus scaled to fit; HUD scaled (percent, 0 = as the menus); whole-number scales only |
| `Borderless` | Borderless fullscreen: a window over the whole monitor, the picture stretched to it with its aspect ratio kept |
| `TimerResolution`, `PreciseFrameLimiter` | 1 ms timer; a precise MaxFPS limiter |
| `FastMenus`, `FastConsole` | One screen copy per menu frame; one screen lock per console frame |
| `FixDirectInput`, `KeyboardMouseOnly` | The DirectInput HID fix of the built-in Interposer; only keyboard and mouse |
| `ServerQueryPorts` | The query ports to ask a game server for its voice and FastDL servers |
| `VoiceEnable`, `VoicePort`, `VoicePassword`, `VoiceHost` | Voice chat; the voice server's port and password; a fixed voice server |
| `VoiceActivation`, `VoiceVolume`, `VoiceMicVolume`, `VoiceMicrophone`, `VoiceShowTalkers` | Mode, volumes, microphone (Options > Sound > Voice chat); who is talking on the HUD |
| `FastDLEnable`, `FastDLURL` | FastDL on joining a server; a fallback URL |
| `RichPresenceEnable`, `DiscordApplicationID`, `DiscordLargeImage` | Rich presence in Discord and the LANCommander launcher |
| `VoiceServer`, `VoiceRange`, `VoiceFullVolumeRange`, `VoiceMaxPlayers`, `VoiceMaxBitrate`, `VoiceBindAddress`, `VoiceAdminKey` | Hosting: the voice server (ranges in metres) |
| `FastDLPort`, `FastDLHost`, `FastDLPath` | Hosting: the FastDL server advertised in the query replies |
| `CrashLog` | Crashes logged with registers and a call chain |
| `PerfLog` | Every 5 seconds, where a frame's time goes (presenting, limiter, screen locks, blits, text) |
| `PlayerName`, `ConnectionSpeed`, `JoinPort` (*game*) | Your player name; the options menu's connection speed; the game port to join |
| `ServerName`, `ServerMaxPlayers`, `ServerRoundTime`, `ServerPurchaseTime`, `ServerMapTime`, `ServerFriendlyFire`, `ServerAllowVoting`, `ServerBalanceTeams`, `ServerDedicated`, `ServerMonsters`, `ServerLANOnly`, `ServerLockedChaseCam`, `ServerSuperWeapons`, `ServerDamageThroughWalls` (*game*) | The Host Game screen's last choices |
| `DisableSound`, `DisableMusic`, `DisableFog`, `DisableLightMap`, `EnableOptimizeSurfaces`, `EnableFixTJunctions`, `EnableTripleBuffer`, `EnableMultiTexture`, `EnableSharpMipmaps` (*game*) | The launcher's sound and renderer switches |
| `Launcher<name>` (*game*) | The launcher's own values, and any other value of the game's registry key |

## Building

Requires (from MSYS2's bash or Git Bash):

- clang and lld-link (`pacman -S mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-lld`)
- Visual Studio 2022 or later with the C++ x86 tools and a Windows SDK
- the LANCommander Interposer's static library, built in `third_party/LANCommander.Interposer` (or `INTERPOSER_DIR`)
  with its `windows-x86` preset (needs `VCPKG_ROOT`):

```
git clone https://github.com/LANCommander/LANCommander.Interposer.git third_party/LANCommander.Interposer
cd third_party/LANCommander.Interposer
powershell -File build/Get-YYThunks.ps1
cmake --preset windows-x86
cmake --build --preset windows-x86-release --target LANCommander.Interposer.Static
cd ../..
sh build.sh
```

Output:
- `build\dinput.dll`: the standalone patch (a DirectInput proxy with the Interposer built in).
- `build\lommpatch.asi`: the same as an ASI plugin.
- `build\LANCommander.Interposer.Plugin.LegendsOfMightAndMagic.dll`: the Interposer plugin.

`INTERPOSER=0 sh build.sh` builds the plugin alone (no Interposer library needed). `LM_VERSION=v1.2.3 sh build.sh`
sets the version the patch writes to its log (`dev` otherwise). GitHub Actions (`.github/workflows/build.yml`) builds
every push and pull request and drafts a release for a `v*` tag.

## Repository layout

| path | contents |
|---|---|
| `src/` | The patch:<br>- `lommpatch.c` (program check, settings, guarded memory writes), `iat.c`, `modules.c` (the engine loading `cshell.dll` / `object.lto` / `d3d.ren` / `server.dll` from temp copies)<br>- `paths.c`, `saves.c`, `settings.c` (console variables in autoexec.cfg), `registry.c`, `input.c` (DirectInput 7 device types for the engine)<br>- `widescreen.c` (Hor+), `window.c` (fullscreen window placement, borderless), `ui.c` (menu and HUD scaling, mouse), `framerate.c` (menus, console, frame limiter)<br>- `voice.c`, `voice_menu.c` (Options > Sound > Voice chat), `voice_keys.c` (voice controls), `voice_game.c`, `voice_net.c` (client), `voice_server.c`, `voice_server_win.c` (server, query replies), `fastdl.c`, `presence.c` (rich presence)<br>- the loaders `dinput_proxy.c`, `asi.c`, `interposer_plugin.c`; `interposer_config.c` (the built-in Interposer's `Config.yml`); `crashlog.c`<br>- `interposer/`: the Interposer's public headers |
| `dist/` | `lommpatch.ini`, shipped with the packages; `voice options.png`, the Voice panel's background (built into the DLLs) |

The game files are not included. You need your own copy of the game.

## License

[MIT](LICENSE). The LANCommander Interposer is MIT licensed too. Legends of Might and Magic itself is not included.
