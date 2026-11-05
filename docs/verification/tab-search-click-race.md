# Tab-search click race

Verified on Windows 11 build 26300, Dia 1.5.0.591 x64.

## Reproduction

1. Open a controlled public Example Domain page, with a matching sidebar tab available.
2. Enable Floating new tab bar. Press Ctrl+T and type `example domain`.
3. Click a matching result. Start Ctrl+L 100 ms after releasing the mouse button.
4. Wait longer than the deferred activation timeout.

Before the fix, this produced the target-unavailable dialog. Temporary stage logging identified the timeout while waiting for the command-bar popover to close. The new address popup was being treated as the old popup.

## Fix

Commit `6f72d00` cancels the older pending activation when Ctrl+L or Ctrl+T starts a newer navigation command. It also schedules restoration of a temporarily revealed sidebar. A normal click still follows the existing activation path.

## Observed results

| Check | Before | After |
| --- | --- | --- |
| Click, then start Ctrl+L after 100 ms | Target-unavailable dialog | No error dialog; newer query preserved |
| Normal clicks at three result positions | Not a complete baseline matrix | Passed; active public addresses checked |
| Rapid Ctrl+L at three result positions | Reproduced failure above | Passed; newer typed input preserved |
| Rapid Ctrl+T at three result positions | Not separately baselined | Passed; newer typed input preserved |
| Keyboard Down, then Enter | Previously working | Existing public tab activated |

The nine position/command cases used a diagnostic build. After removing all temporary diagnostics, the original Ctrl+L reproduction was repeated on the clean build `v1.0.1-3-g6f72d00`. It passed with five matching rows, no error dialog, and the typed sentinel unchanged after 4.2 seconds.

Clean DLL SHA-256:

`81242637AF6E7EB4E9CCC1DBFE713169F496A219D9A446E96771B2EB5EEA8BFE`

The native MSVC build and active LSP check of the including `ui.cpp` translation unit passed. No test files or browser data files were added.

These checks establish the reproduced stale-activation fix, not complete multiwindow/focus, floating-sidebar or split-view coverage. Sidebar blinking, incomplete Favorites, accessibility and privacy-policy review remain separate concerns. The earlier failure image in the PR is historical before-fix evidence.
