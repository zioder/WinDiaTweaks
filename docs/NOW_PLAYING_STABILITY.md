# Now Playing stability (#52)

Issue: https://github.com/zioder/WinDiaTweaks/issues/52

The report describes intermittent hangs, crashes and mouse stutter. These changes address mechanisms
found in source inspection; they do not establish which one caused the reporter's failures.

## Changes

- All Core Audio discovery, session enumeration and peak reads run on a dedicated MTA worker. This is
  separate from the SMTC worker, so a stalled metadata/artwork request cannot freeze the equalizer.
- The UI reads a lock-free, timestamped numeric sample. After 250 ms without a fresh sample it uses the
  existing gentle pulse. There is no UI wait for audio work. The worker releases its meters and sleeps
  after 500 ms without demand; hidden cards do not request audio.
- Asynchronous SMTC calls have two-second deadlines. A synchronous COM call may still stall its worker;
  the UI does not join that worker or hold an audio lock. Playback commands are bounded and tied to the
  session that the clicked card showed, and expire after two seconds, so delayed commands cannot target
  a replacement session or replay a long backlog.
- Hosts and event callbacks use shared/weak ownership. Teardown marks hosts and cards inactive before
  touching XAML, independently revokes rendering and size subscriptions, stops equalizers and cancels
  pending artwork. Late callbacks are inert even if XAML retains a removed control.
- Artwork requests have a generation and session/title identity. Song changes invalidate/cancel old
  decodes; removed cards reject their completions. Artwork input is capped at 8 MiB before decoding.
- Sidebar and tab-row discovery use weak caches invalidated by tree changes, with a two-second fallback
  scan. Titles, sound state, selection, icons and colors still refresh on the existing 300 ms tick.
  Stationary cards update progress without rewriting all their geometry; hidden stacks stop frame work.
- Go-to-tab taps are serialized, check the foreground/target window immediately before injection, use
  valid touch pressure, and always attempt to release a successful contact. Cursor restoration is
  conditional on the cursor still being at the injected point in the target foreground window. Failed
  touch-downs do not move the pointer. Synthetic touch remains a compatibility workaround, not a native
  tab-selection API; movement and focus races still need manual verification.
- `tap.log` records slow audio discovery/samples (100 ms), asynchronous requests (100 ms or timeout),
  and UI updates (50 ms). Repeated slow sample/UI logs are rate-limited. No new network telemetry is sent.

## Automated checks

Run `build.cmd test-media` from a checkout with the C++ build tools installed. It covers:

- Missing, invalid, clamped, fresh and expired audio levels.
- A deliberately blocked producer while 100,000 UI reads and requests continue.
- Concurrent requests from several UI threads, retaining the furthest demand deadline.
- Coherent timestamp/level publication while a producer changes both repeatedly.

Run `build.cmd` for the full DLL and launchers. Build artifacts, generated WinUI headers and local
settings stay in ignored directories.

## Manual playback checks

From the repository root, serve `tests` locally:

```powershell
python -m http.server 8765 --bind 127.0.0.1 --directory tests
```

For an isolated checkout test, run `bin\diatweaks.exe stop-helper` to pause any installed auto-attach
helper, then `bin\diatweaks.exe enable mediaControls` and `bin\diatweaks.exe launch`. Launch closes
an existing Dia instance. Restore the helper afterward with `diatweaks.exe sync-helper` from its original
installed folder. In an extracted test package, the executables are at the package root instead of `bin`.

Open `http://127.0.0.1:8765/media_fixture.html` in Dia with this build and enable **Media Controls**.
The fixture generates a quiet tone and artwork locally. Start its audio with the play control, then
switch to another tab so its Now Playing card appears.

1. Hover/unhover the card, pause/resume, seek, next/previous, mute/unmute and dismiss. Check the equalizer,
   artwork, title, artist, progress and existing animations.
2. Use **Change song 20 times**, switch away, and repeat while toggling Media Controls off/on. Old
   artwork must not return, removed cards must not reappear, and the browser must remain responsive.
3. Close the playing tab or its window during a song change and during expansion/exit animations.
   Reopen the fixture; verify controls still work and no repeated callback errors appear in `tap.log`.
4. Hide, dock and float the sidebar; resize the window; change spaces and use split view. The card must
   follow its playing tab and make the same room beside the space switcher.
5. Click go-to-tab repeatedly, move the pointer during the action, and switch foreground applications.
   Taps must not overlap, target another foreground window, or restore the cursor over observed movement.
6. Repeat with two audible tabs, a pinned tab, Spotify/YouTube, and the PiP option. Inspect `tap.log`
   for slow UI updates and Windows Application events for crashes. An overnight/user reproduction run
   is still needed for the intermittent report.

## Validation limits

On Windows with Dia 1.6.0.528, the initial diagnostic build attached and added the media card area.
Automated shared-state tests and the full build passed. Live playback, screenshot comparison, input
races and the UI matrix above were not completed: the desktop automation tool rejected Dia browser
interaction because URL-policy enforcement is unsupported for that browser. These checks remain a
release gate; build/test success alone does not prove issue #52 resolved.
