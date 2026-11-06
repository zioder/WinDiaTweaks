# WinDiaTweaks — porting notes

Goal: match [JaceThings/DiaTweaks](https://github.com/JaceThings/DiaTweaks) (macOS) on Windows —
unofficial UI tweaks for the Dia browser. Nothing in Dia's install folder is modified; uninstall = delete this folder.

Target: `TheBrowserCompany.Dia` 1.5.0.588, x64, packaged (MSIX), Swift + WinUI 3.

The opt-in `localBrowsing` fallback discovers and validates native capabilities in each installed image. See [COMPATIBILITY.md](COMPATIBILITY.md) for the update policy and [LOCAL-BROWSING.md](LOCAL-BROWSING.md) for its behavior and checks.

## Use

Open Dia through `bin\diatweaks.exe`, then **Dia → Settings → Patches**: every tweak has a switch there
(plus a drop-down or text box where it has an option). Appearance changes apply instantly and are saved to `bin\tweaks.conf`; the forced sign-in bypass requires a restart.

The command line does the same:

```
bin\diatweaks.exe                  :: start Dia with tweaks (or reload if already running)
bin\diatweaks.exe list
bin\diatweaks.exe enable hideToolbar
bin\diatweaks.exe set windowMaterial acrylic
bin\diatweaks.exe disable all
bin\diatweaks.exe status
bin\diatweaks.exe dump             :: writes bin\tree.txt (Dia's UI tree, for making tweaks)
bin\diatweaks.exe probe Name Name* :: describes live elements (for making tweaks)
```

Tweaks need to be re-attached each time Dia starts: open Dia through `bin\diatweaks.exe`.

Build: `build.cmd` (close Dia first). It needs VS 2022 C++ tools + Windows SDK. The first build runs
`gen-winrt.cmd`, which makes C++/WinRT headers from the WinUI metadata inside Dia's own install
(`winmd\`, `gen\`); re-run it after a Dia update if the build complains.

## How it works

`diatweaks.exe` loads `DiaTweaksTap.dll` into the running Dia once; the DLL attaches through WinUI's XAML
diagnostics API. From there:

- `src/tap.cpp` tracks Dia's live UI tree and applies the property rules in `tweaks.conf`, putting Dia's
  own values back when a tweak is turned off.
- When Dia changes a property a tweak overrides (e.g. its page inset while the sidebar animates), WinUI
  notifies the engine synchronously and the tweak's value goes straight back before the frame is drawn,
  so there's no flash. Dia's new value is remembered so turning the tweak off restores it.
- `src/ui.cpp` uses real WinUI objects (C++/WinRT) for the **Settings → Patches** page (a NavigationViewItem
  added to Dia's Settings NavigationView, page built in Dia's card style), the window states rules can use
  (`sidebarOpen`, `newTab`, `showToolbar`), and the tweaks that need code.
- **Hidden toolbar** (and the three "show toolbar" tweaks) is done in code: a watcher thread checks the
  pointer every 8 ms and pings Dia's UI thread at once. Showing/hiding slides the toolbar *and the web page*
  together each frame (ease-out, 200 ms). While sliding, the page keeps its size, so Dia only moves its
  web-content window; the page is resized once, at the end of a reveal or the start of a hide. This is the
  sliding, not reflowing every frame. Measured: ~20 positions per slide,
  first movement ~10-25 ms after the pointer reaches the top. Typing in the address bar (Ctrl+L) also keeps
  the toolbar out.
- With the toolbar hidden (Hide toolbar / Arc URL bar), Dia's invisible window-drag strip over the old toolbar
  (`BoostBrowser.WindowCaptionInputSink`) is reshaped to cover only the sidebar, so the top of the page takes
  clicks; Dia's own shape comes back when the toolbar shows. On Dia's start page (New Tab), which is drawn in
  XAML, the toolbar slide runs as a compositor translation with the page height kept constant, so it doesn't
  re-lay out the start page every frame.
- **Safe mode** (as on macOS): if Dia closes twice in a row within a minute of starting with tweaks, the next
  start runs with tweaks off and Settings → Patches shows a "Turn patches back on" button.

## Tweaks — macOS list vs Windows

Verified in Dia on this machine with screenshots (on, then off again, unless noted).

| macOS tweak | Windows key | Status |
|---|---|---|
| Edge-to-edge web content | `edgeToEdge` | Done. Dia only adds the gap in some window states; maximized has none. |
| Square corners by the sidebar | `squareCorners` | Done, verified (normal and maximized window). Squares Dia's rounded UI corners around the page. The web page's own bottom corners are rounded inside Dia's engine (`roundsContentCorners` in Dia's Swift code), so the page is stretched ~9 px past the bottom of the window and Windows clips the rounded corners off; the last ~9 px of the page sit below the window edge. |
| Hide toolbar | `hideToolbar` | Done, verified. The window buttons fade out with the toolbar and back in when it's revealed; while hidden they can't be clicked by accident. |
| Reveal toolbar on hover | *(merged into `hideToolbar`)* | Done, verified. Hide toolbar now always slides the toolbar back in while the pointer is at the top edge. |
| Show toolbar with the sidebar | `showToolbarWithSidebar` | Done, verified (sidebar open → shown, closed → hidden). |
| Show toolbar on new tab pages | `showToolbarOnNewTab` | Done, verified (Dia's start page). |
| *(new)* Toolbar color | `toolbarColor` | Done, verified (normal and hidden toolbar; hover on buttons, the address field and Chat; Adaptive ↔ Sidebar). Drop-down: Adaptive (Following Site) = Dia's own, Sidebar. Dia keeps each toolbar's site palette in the strip's own resources (`NavigationBarBackgroundBrush`, `ButtonBackgroundPointerOver`, `SubtleFillColorSecondaryBrush`, `ControlFillColorDefaultBrush` for Chat, ...); with Sidebar those brushes are recolored to the app's values for the same keys (the strip itself: see-through), so hover and pressed states look like the sidebar's. The address field's hover fill is its own brush and is swapped for the app's subtle fill; icons and window buttons take the sidebar's text color. Dia's colors come back with Adaptive. `probe` now also lists an element's own brush resources. |
| *(new)* Preset | `preset` | Done, verified. Drop-down at the top of Settings > Patches: None (all patches off, the default), Arc (edge-to-edge, square corners, hide toolbar with reveal on hover, Arc URL bar, floating new tab bar, hide tab search button, hide Chat button, window material = Acrylic; everything else off), Custom (your own mix). Picking None or Arc sets every patch in one write; changing any patch by hand switches the preset to Custom. "Turn all off" = None. |
| *(new)* Arc URL bar | `arcUrlBar` | Done, verified. Header row: Dia logo, sidebar toggle, back/forward/reload (Dia's ⌄ menu kept at the end); all icons in one row at Dia's own button height, same color, the logo drawn like the other icons, the toggle right next to the logo, and the nav buttons against ⌄ (or the edge, at the logo's margin, when ⌄ is hidden); the address pill directly below; top toolbar hidden. Dia's real elements are moved (single-toolbar style). Dia keeps a toolbar per tab; the bar takes over the new tab's controls the moment Dia inserts them, on the next frame (~16-23 ms), restyled in the same pass. The pill shows only the site (search pages: Dia's "Google /" chip + query); the page's action buttons (☆ / ⋯ or link / site settings) fade in at the far right only while the pointer is over the pill. Icons use the sidebar color, not Dia's per-site tint. In split view it follows the selected pane (the one with keyboard focus) and stays put while focus is in the sidebar, so it no longer flips between panes. Each pane's own toolbar keeps only its close (X) button; the selected pane's controls live only in the sidebar. The sidebar toggle stays visible whichever pane is selected (Dia normally shows it only in the leftmost pane). Clicking the pill or Ctrl+L opens Dia's command bar: anchored over the pill at Arc's ~600 px width while the sidebar is docked; centered over the page (760 px wide, 20% from the top, above the floating sidebar) while the sidebar is closed. Enter/Go navigates. Ctrl+T is Dia's own New Tab unless the Floating new tab bar patch is on. |
| *(new)* Hide tab search button | `hideTabSearchButton` | Done, verified. Hides the ⌄ (search tabs) button at the top of the sidebar; with the Arc URL bar, back/forward/reload move into its place. |
| *(new)* Floating new tab bar | `floatingNewTab` | Done, verified. Ctrl+T shows Dia's command bar floating over the current page, empty and centered (the same bar as Ctrl+L), instead of a New Tab page. Enter opens a new, selected tab with the URL (~0.3 s): Dia has no 'open in new tab' key (Enter = this tab, Alt+Enter = split view, Shift+Enter = new line, Ctrl+Shift+Enter = search here), so the patch opens Dia's own new tab, keeps its New Tab artwork hidden, and submits the text in that tab's box once Dia has read it. Esc creates nothing. Dia's New Tab page itself is unchanged. |
| Always show address field | `alwaysShowAddressField` | Done, verified. Field background + bookmark/site buttons; Dia's hover binding is restored when off. |
| Hide sidebar button | `hideSidebarButton` | Done, verified. |
| Hide Chat button | `hideChatButton` | Done, verified (also from the Settings switch). |
| Hide Chats button / Hide recent tabs button | — | Dia for Windows has no such buttons. |
| Profile emoji | `profileEmoji` | Done, verified. One emoji (set in Settings → Patches) at the top of the sidebar; not per-profile. |
| Hide pinned tab border | `hidePinnedTabBorder` | Done (applied to all pinned tabs; not seen with a pinned tab selected). |
| Translucent sidebar | `translucentSidebar` | Done, verified. |
| Don't pop up downloads | `dontPopUpDownloads` | Implemented, **not verified**: needs a real download to test. Closes a downloads pop-up that opens with no click or key press just before it. |
| Close tabs from the tab switcher | — | Dia for Windows has no tab-switcher UI (Ctrl+Tab switches directly). |
| Keep windows active | — | Dia for Windows looks identical active and inactive (checked pixel by pixel), so there is nothing to keep. |
| macOS 26 window corners | — | macOS-only (Windows 11 rounds windows itself). |
| Hide traffic lights | `hideWindowButtons` | Done, verified. Hides minimize/maximize/close *and* removes their click areas (Dia registers them with Windows, so a hidden button could otherwise still be clicked); Alt+F4 still closes. |
| Window material | `windowMaterial` | Done, verified. Mica, Mica Alt or Acrylic behind the tabs and sidebar. |
| Allow iCloud passkeys / iCloud Passwords / 1Password | — | macOS-only. |
| Group Tabs by Domain (command) | — | Not done: Dia exposes no way to reorder tabs other than dragging them. |

The macOS Settings → Patches page is ported (switches, options, appearance presets and safe mode).
The Windows implementation also has its own Updates page. It never modifies Dia's installed files;
turn appearance tweaks off to restore their values, or disable local browsing and restart to remove
its process-local changes.

## Files

- `src/tap.cpp` — tree tracking, rule engine, config, safe mode. `src/engine.h` — API shared with ui.cpp.
- `src/ui.cpp` — Settings → Patches page, window states, code-driven tweaks, `probe`.
- `src/diatweaks.cpp` — the command.
- `src/local_browsing.cpp` — guarded native startup and the process-local patch transaction.
- `src/dia_native_layout.cpp` — read-only PE/Swift capability discovery, without a version allowlist.
- `src/dia-load.cpp`, `src/test_attach.cpp`, `src/launch_direct.cpp` — earlier experiments, not needed.
- `build.cmd`, `gen-winrt.cmd`, `reload.ps1` (close Dia, rebuild, restart with tweaks).
- `tweaks.conf` — tweak definitions (copied to `bin\` on first build; `bin\tweaks.conf` is the live one).
