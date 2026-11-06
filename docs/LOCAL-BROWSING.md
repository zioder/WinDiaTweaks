# Bypassing forced sign-in for local browsing

The **Bypass forced sign-in** setting (`localBrowsing`) allows ordinary browsing without completing Dia's required sign-in, including when an unsupported region prevents signing in. It is opt-in, requires a restart, and is independent of appearance presets. It follows the repo's [capability based compatibility policy](COMPATIBILITY.md).

## Build admission and lifetime

The launcher binds the executable to Dia's registered package family, then inspects `Dia.dll` before closing an existing browser. The read-only resolver uses:

- PE sections, function ranges and imported Swift runtime functions.
- Reflected type, field and enum names and their links to the relevant code.
- Diagnostic anchors and decoded native branches, cleanup and readiness checks.
- The existing signed-out command array and sufficient unused capacity for the local actions.

All sites and controller offsets are discovered from the inspected image. A missing, changed or ambiguous capability is rejected; there is no version, hash, timestamp or module-offset allowlist. The source file is held open against replacement during launch. Its measured SHA-256 must match the debugger's actual loaded-file handle, and loaded instructions and runtime Swift metadata must match the resolved layout. The hash establishes freshness, not release support. Unsupported capabilities fail before the current browser is closed.

No package, profile, token, history or password files are patched. Native instruction and allowlist changes belong to the new Dia process and disappear when it exits. The temporary debugger detaches after successful initialization; it does not stay running. Unrecoverable startup/control failures terminate only the process created by that launch, after rollback is attempted.

The launcher sets `_NO_DEBUG_HEAP=1` in the child's inherited environment. Otherwise, debugger-assisted creation enables the Windows debug heap for the browser's lifetime, even after detach. All other inherited entries are preserved, and the parent environment is unchanged. This avoids [Windows debug-heap overhead](https://devblogs.microsoft.com/oldnewthing/20130103-00/?p=5653/); it does not limit the memory used by pages, extensions or the GPU process.

## Startup

Dia initializes its browser engine before waiting for authentication. A one-shot interception at the resolved authentication-result branch takes its existing cleanup path. That path returns the internal `restored` initialization reason, discovered from the enum metadata; the account controller retains its actual state. Shared initialization, local user data, browser contexts and window restoration then run normally.

The interception requires the expected controller metadata, an initialized engine, a non-terminating application, existing auth/window controllers and the observed no-error, signed-out restore result. It runs once, using the native continuation and cleanup rather than calling Swift async code as a C function. Restoration errors that take a different path are not silently treated as authenticated.

Neither `--onboarding-force-complete` nor `--onboarding-skip-wizard` is passed. Manual testing showed those flags alone leave Welcome in place; static tracing also showed that force-complete can skip normal window restoration after the authentication wait is removed.

## Local commands and windows

Account state remains unchanged. The native changes are limited to local browsing checks:

- Window creation keeps its engine, termination, context-loading and incognito policy checks, while dropping its account requirement.
- The existing signed-out application-action allowlist gains exactly five entries: Settings, Set as Default Browser, Import from Another Browser, New Window and New Incognito Window. The static object's metadata/refcount header and following object remain untouched.
- New-window availability retains the original engine and launch-state checks, with native retain/release cleanup intact.
- Print keeps its focused-content and printable-target checks. A disabled Print action on a New Tab is not evidence of an account restriction.

The global authentication getter, credentials, server responses, account models and AI entitlements are not replaced. Startup analytics may label the local initialization as `restored`, reflecting the internal continuation reused by the fallback.

## Automatic revocation

The automatic session-revocation selector uses Dia's existing permissive dialog, with **Sign Out** and **Not Now**, instead of immediately tearing down windows. Selecting **Sign Out** runs Dia's original sign-out path. Manual sign-out is separate and is retained.

This applies to automatic account-session revocation generally. The exact mapping from a server's restricted-country response to a later revocation was not established, so it is not a country-specific error rewrite. The original diagnostic label can still describe the incoming revocation as strict even though local browsing mode presents the choice.

## Verification

The native test fixture runs the production debugger and patch transaction against an owned, non-browser child process. It covers supported-byte checks, mismatches, write/cache/protection failures, rollback, debugger detach and context-restoration failures, cooperative interruption, and console cleanup while callbacks are active. It does not read user browser data.

The resolver fixture rejects malformed PE images. Given local `Dia.dll` paths, it also tests the real layouts and targeted mutations, including changed control flow, occupied array padding and ambiguous Swift descriptors. A timestamp change is explicitly accepted. Vendor images are not included in the repository.

```powershell
cmd /c tests\local_browsing_native.cmd
cmd /c tests\dia_native_layout_native.cmd
cmd /c build.cmd
powershell -File tests\local_browsing_config.ps1
.\bin\diatweaks.exe enable localBrowsing
.\bin\diatweaks.exe restart
```

Manual acceptance in a signed-out session where Dia requires sign-in, including the unsupported-region scenario:

1. A normal window opens instead of Welcome; a page loads, Ctrl+T works and tabs switch.
2. Settings, default-browser setup and the import dialog are available. Opening these dialogs is sufficient; importing data or changing the default browser is unnecessary for the check.
3. New windows and incognito windows work after the launcher exits, subject to the normal incognito policy.
4. Print is available on a loaded, printable page.
5. A fresh launch through `diatweaks` still works with the setting saved.
6. Disabling the setting and restarting restores Dia's normal account gate.
7. When an actual automatic revocation can be reproduced, **Not Now** preserves existing tabs while rejected account/AI services remain rejected. This event is distinct from testing signed-out startup.

The user verified page navigation, new tabs, tab switching, all five restored local menu actions, and printing a loaded page on Dia **1.3.1.2 x64** after debugger detach. A fresh restart with the final capability resolver was also verified for ordinary tabs and Settings. Dia **1.4.0.424 x64** was subsequently installed and launched successfully through the resolver, opening an ordinary browser window with the changelog tab. Its full local-menu acceptance checks remain pending. Automatic server-triggered revocation has not been reproduced here.
