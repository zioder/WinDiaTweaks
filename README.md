# WinDiaTweaks


<img width="2557" height="1597" alt="image" src="https://github.com/user-attachments/assets/5331dcf8-cc89-49fb-889d-0d0a1ab7427a" />


Customize how [Dia](https://www.diabrowser.com) looks on Windows, with every option right inside Dia's Settings.

Unofficial and not affiliated with The Browser Company. Windows 11 x64, Dia for Windows (tested with 1.5.0.588).

Inspired by [DiaTweaks](https://github.com/JaceThings/DiaTweaks) for macOS by [Jace](https://x.com/JaceThings). WinDiaTweaks brings those tweaks to Dia for Windows, rebuilt from scratch for its WinUI interface, and adds features of its own on top: an Arc-style URL bar in the sidebar, a floating Ctrl+T bar, presets and more.

## Tweaks

Switch tweaks in **Dia → Settings → Patches** or from the `diatweaks` command. Appearance changes apply instantly. The experimental forced sign-in bypass applies when Dia restarts.

At the top of **Patches**, **Preset** sets the appearance tweaks at once, keeping your browsing, launch and update settings:

- **None**: Dia's original appearance (the default).
- **Arc**: the address bar and buttons move into the sidebar, the toolbar hides, pages go edge to edge with square corners, Ctrl+T opens a floating bar, tab hover cards stop popping up, the window turns acrylic.
- **Custom**: your own mix. Changing any tweak switches to it.

| Tweak | Does |
|---|---|
| **Browser** | |
| Bypass forced sign-in (experimental) ✨ | Opens ordinary browser windows without completing Dia sign-in. Requires a restart and compatible native capabilities; see below |
| **Page** | |
| Edge-to-edge web content | Removes the gap between pages and the window edge |
| Square corners by the sidebar | Pages meet the sidebar and window edges with straight corners |
| **Toolbar** | |
| Hide toolbar (reveal on hover) | Hides the toolbar; it slides back in while the pointer is at the top of the window |
| Show toolbar with the sidebar | With the toolbar hidden, shows it while the sidebar is open |
| Show toolbar on new tab pages | With the toolbar hidden, keeps it on new tab pages |
| Toolbar color ✨ | Adaptive (follows the site, Dia's default) or Sidebar (same color or window material as the sidebar) |
| Arc URL bar ✨ | Moves the sidebar button, back, forward, reload and the address into the top of the sidebar, like Arc. The address shows just the site; its buttons appear on hover |
| Floating new tab bar ✨ | Ctrl+T shows the address bar floating over the page instead of a New Tab page; Enter opens the result in a new tab |
| **Toolbar items** | |
| Always show address field | Keeps the address field's background and buttons visible, not just on hover |
| Hide sidebar button | Hides the sidebar button |
| Hide tab search button ✨ | Hides the ⌄ button at the top of the sidebar |
| Hide Chat button | Hides the Chat button |
| Hide page loading animation ✨ | Hides the orange bar that sweeps across the address field while a page loads |
| **Sidebar & tab bar** | |
| Hide pinned tab border | Removes the colored border around the selected pinned tab |
| Hide tab hover card ✨ | Stops the card with the page title, site and buttons from popping up when you hover a tab or pinned tab |
| Profile emoji | Shows an emoji for this profile at the top of the sidebar |
| Don't pop up downloads | Stops the downloads list from opening by itself for each download |
| Translucent sidebar | Lets the page show through the sidebar when it floats over the page |
| **Window** | |
| Hide window buttons | Hides minimize, maximize and close (Alt+F4 still closes) |
| Window material | Mica, Mica Alt or Acrylic behind the tabs and sidebar |

✨ New in WinDiaTweaks, not in the macOS original. How each one is done on Windows: [docs/PORTING.md](docs/PORTING.md).

### Bypassing forced sign-in

**Bypass forced sign-in (experimental)** lets you use ordinary tabs without completing Dia's required sign-in. This includes the case where a region or account restriction sends Dia back to Welcome. The launcher discovers and checks the required native capabilities in your installed Dia. Missing or ambiguous capabilities produce an error before an existing browser is closed. Compatibility is not restricted to a version or file hash; see the [compatibility policy](docs/COMPATIBILITY.md).

Enable it under **Settings → Patches → Browser**, then choose **Restart Dia**. If Welcome prevents you from reaching Settings, run these commands from this checkout:

```powershell
.\bin\diatweaks.exe enable localBrowsing
.\bin\diatweaks.exe restart
```

AI, sync and account services retain their actual availability. The setting restores local actions such as Settings, imports, and new or incognito windows, while keeping their usual browser and policy checks. Automatic account-session revocation offers Dia's existing **Sign Out / Not Now** choice; **Not Now** keeps browsing open. Manual sign-out keeps its normal behavior.

This setting is off by default and is kept when you change appearance presets. `enable all` leaves it unchanged; enable it by name. To remove it, run `diatweaks disable localBrowsing`, then restart. The changes exist only in the running process; Dia's installed files and account credentials are not edited. Technical details and verification steps are in [docs/LOCAL-BROWSING.md](docs/LOCAL-BROWSING.md).

For future starts, use **Dia (Tweaks)** when Dia is fully closed. **Patch Dia however it's opened** can add appearance tweaks to an ordinary launch, but the sign-in bypass needs the tweaked launcher to start the process.

## Install

Install [Dia](https://www.diabrowser.com), then paste this into PowerShell:

```powershell
irm https://github.com/zioder/WinDiaTweaks/releases/latest/download/install.ps1 | iex
```

It downloads the newest [release](https://github.com/zioder/WinDiaTweaks/releases) into `%USERPROFILE%\WinDiaTweaks\bin`, adds **Dia (Tweaks)** to the Start menu and the desktop and reopens Dia with the tweaks. Open Dia from **Dia (Tweaks)** from now on, then go to **Settings → Patches**.

### Build it yourself

Prefer to compile it from the code here?

1. Install [Dia](https://www.diabrowser.com) and [Git](https://git-scm.com/download/win).
2. Install Microsoft's C++ build tools (free). In PowerShell:

   ```powershell
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```

   Already have Visual Studio 2022 with **Desktop development with C++**? Skip this.
3. Paste this into PowerShell:

   ```powershell
   git clone https://github.com/zioder/WinDiaTweaks.git "$HOME\WinDiaTweaks"; & "$HOME\WinDiaTweaks\install.cmd"
   ```

It builds WinDiaTweaks on your PC, adds **Dia (Tweaks)** to the Start menu and the desktop and reopens Dia with the tweaks. At the end it asks whether you'd like to star the repo (y/n): with the [GitHub CLI](https://cli.github.com) signed in it stars it for you, otherwise it opens the repo page. Open Dia from **Dia (Tweaks)** from now on, then go to **Settings → Patches**.

## Two ways to open Dia with the tweaks

The tweaks have to be added each time Dia starts. Pick how:

| | **Dia (Tweaks)** shortcut (the default) | **Normal Dia** (opt-in) |
|---|---|---|
| How you open Dia | **Dia (Tweaks)** in the Start menu or on the desktop | Any way: Dia's own icon or pin, the Start menu, a link from another app |
| Runs in the background | Nothing | A small helper (`DiaLauncher.exe`), from sign-in |
| Tweaks show | From Dia's first frame | From Dia's first frame |
| Opening Dia any other way | Plain Dia, no tweaks | Tweaks too |
| Turn on | Nothing to do | **Settings → Patches → Opening Dia → Patch Dia however it's opened**, or `diatweaks auto on` |

### Dia (Tweaks) shortcut

What you get after installing. Open Dia from **Dia (Tweaks)**: it starts Dia the way the Start menu does, adds the tweaks while Dia is starting, and closes. Dia opens as fast as without WinDiaTweaks and comes to the front. If Dia is already open, it brings Dia forward, and adds the tweaks if they're missing. Nothing keeps running; open Dia any other way (its own icon, or a link from another app when Dia is your default browser) and you get plain Dia.

Pin **Dia (Tweaks)** to the taskbar instead of Dia if you use the taskbar.

### Normal Dia

For when you'd rather keep opening Dia the usual way, or Dia is your default browser and opens from links. Switch on **Patch Dia however it's opened** in **Settings → Patches → Opening Dia**, or in PowerShell:

```powershell
& "$HOME\WinDiaTweaks\bin\diatweaks.exe" auto on
```

A small helper, `DiaLauncher.exe --watch`, then runs from sign-in (it's in your Startup folder as **WinDiaTweaks**). It notices Dia the moment it starts, however it was opened, and adds the tweaks before Dia's window shows. It never opens, closes or restarts Dia and needs no administrator rights. It waits for Windows to report new windows rather than checking constantly: about 17 MB of memory and well under 1% of one CPU core. Problems are written to `bin\watch.log`.

Switch it off the same way, or with `diatweaks auto off`: the helper stops and leaves sign-in. `diatweaks auto status` shows whether it's on and running. **Dia (Tweaks)** keeps working either way.

## The `diatweaks` command

`diatweaks.exe` is in the `bin` folder. Run it on its own to open Dia with the tweaks (or reload them if Dia already has them), or:

| Command | Does |
|---|---|
| `diatweaks list` | Shows every tweak and whether it's on |
| `diatweaks enable hideToolbar` | Turns a tweak on; Dia changes straight away |
| `diatweaks disable hideToolbar` | Turns a tweak off (`all` works too) |
| `diatweaks set windowMaterial acrylic` | Sets a tweak's option |
| `diatweaks status` | Shows whether Dia is running with the tweaks, and which are on |
| `diatweaks restart` | Closes Dia and opens it again with the tweaks |
| `diatweaks attach` | Loads patches into running Dia without opening or restarting it |
| `diatweaks auto on` | Adds the tweaks however Dia is opened ([Normal Dia](#normal-dia)); `auto off` stops that, `auto status` shows it |
| `diatweaks shortcuts` | Adds **Dia (Tweaks)**, with Dia's icon, to the Start menu and the desktop again |
| `diatweaks check` | Shows the installed and newest versions of Dia and WinDiaTweaks |
| `diatweaks update` | Installs updates to Dia and WinDiaTweaks (or `update dia`, `update tweaks`) |

You can name a tweak by its key (`hideToolbar`) or by the title shown in Settings (`"Hide toolbar (reveal on hover)"`).

## Updating

**Settings → Patches → Updates** shows the version of Dia and of WinDiaTweaks you have, and whether there's a newer one. It checks when Dia opens (switch off **Check for updates automatically** to only check when you press **Check now**), puts a badge on **Patches** when something is new, and shows what changed in the newest WinDiaTweaks. **Update** closes Dia, installs the update in a small window and reopens Dia with your patches; your choices are kept.

- **Dia** updates from the same channel it was installed from (stable or release candidate).
- **WinDiaTweaks** updates from the newest [release](https://github.com/zioder/WinDiaTweaks/releases). If you built it yourself, it runs `git pull` and rebuilds it instead.

Or from PowerShell: `diatweaks check` to see what's new, `diatweaks update` to install it (`diatweaks update dia` or `diatweaks update tweaks` for one of them).

Dia updates keep your choices. The local browsing fallback checks the updated image on its next launch; a change to a required native capability can require a WinDiaTweaks update. See [Compatibility](docs/COMPATIBILITY.md).

### Installed before v1.0.0?

Versions before v1.0.0 don't have **Updates** yet, so update once by hand. In PowerShell:

```powershell
cd "$HOME\WinDiaTweaks"; git pull; .\install.cmd
```

It closes Dia, rebuilds WinDiaTweaks and reopens Dia. Your patches stay as they were, and tweaks added since show up switched off. From then on, use **Settings → Patches → Updates**.

Building it yourself, if the build complains after Dia updates, delete the `gen` and `winmd` folders and run `install.cmd` again: they're made from the Dia you have installed.

## FAQ

<details>
<summary><strong>Is it safe?</strong></summary>

Releases are built by [GitHub Actions](.github/workflows/release.yml) from the code in this repo, and you can compile it yourself instead. Appearance tweaks rearrange and restyle Dia's interface. The optional forced sign-in bypass also changes native startup and local-command checks in memory, after capability and runtime checks. It does not load or edit account credentials. Your choices live in one plain text file, `bin\tweaks.conf`.

</details>

<details>
<summary><strong>How does it work?</strong></summary>

Nothing in Dia's install folder is changed. `diatweaks.exe` opens Dia and loads a small add-on into it. The add-on uses WinUI's XAML diagnostics interface, the same one Visual Studio's Live Visual Tree uses, to move, hide and restyle parts of Dia while it runs. Turning an appearance tweak off puts Dia's own values back. The forced sign-in bypass uses an early native startup hook and requires a restart when switched on or off.

</details>

<details>
<summary><strong>Does it run in the background?</strong></summary>

Not by default. With the **Dia (Tweaks)** shortcut nothing stays running after you close Dia, and nothing starts with Windows. If you open Dia some other way than **Dia (Tweaks)** or `diatweaks`, you simply get Dia without the tweaks.

If you switch on [Normal Dia](#normal-dia), one small helper runs from sign-in so the tweaks are added however Dia opens. Switch it off and it stops.

</details>

<details>
<summary><strong>My antivirus complains.</strong></summary>

Loading an add-on into another app is something antivirus software watches for. It's built from this code, so you can read exactly what it does. Allow `bin\diatweaks.exe`, `bin\DiaLauncher.exe` and `bin\DiaTweaksTap.dll` if it's blocked.

</details>

<details>
<summary><strong>What if Dia keeps crashing?</strong></summary>

WinDiaTweaks watches for that: after two launches in a row where Dia closes almost immediately, the next launch skips the tweaks (safe mode). Once things are sorted, re-enable them from the button at the top of **Settings → Patches**.

</details>

## Uninstall

Run `uninstall.cmd` in the WinDiaTweaks folder (`bin` for an installed release). It removes the Start menu and desktop shortcuts, stops the Normal Dia helper and removes it from sign-in, and reopens Dia without the tweaks. Then delete the folder.

## Credits

The idea, the tweak list and the Settings → Patches design come from [DiaTweaks](https://github.com/JaceThings/DiaTweaks) by [Jace](https://x.com/JaceThings). Thank you! WinDiaTweaks is an independent Windows version and isn't maintained by Jace.

Dia is a trademark of The Browser Company. This project isn't affiliated with, endorsed by or supported by The Browser Company. Please don't report WinDiaTweaks problems to them; [open an issue here](https://github.com/zioder/WinDiaTweaks/issues) instead.

## Support

If WinDiaTweaks is useful to you, you can support its development:

- [GitHub Sponsors](https://github.com/sponsors/zioder)
- [Buy Me a Coffee](https://buymeacoffee.com/zioder)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE)
