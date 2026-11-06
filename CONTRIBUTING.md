# Contributing

Thanks for helping out. The short version: small changes, checked by hand in Dia, with a screenshot.

## Opening a pull request

- Keep each PR to a single change. Two unrelated fixes are two PRs.
- Use the template (What, Why, Proof, Tested on, Checklist) and answer every part briefly. Write `N/A` where something doesn't apply rather than removing it.
- Anything you can see in Dia needs proof: a screenshot, or a short recording if it moves. If you change how an existing tweak behaves, show it before and after.
- Look for the ways your change could go wrong before submitting: turn the tweak on and off, dock, close and hover the sidebar, open split view and the New Tab page.
- Build with `build.cmd` (`reload.ps1` builds and restarts Dia in one go) and use it in your own Dia for a while.

## How it's put together

| File | What it is |
|---|---|
| `tweaks.conf` | Every tweak: title, description, section, icon, option, and its property rules |
| `src/tap.cpp` | The engine inside Dia: tracks Dia's UI tree, applies the rules, puts Dia's values back |
| `src/ui.cpp` | Settings → Patches, and the tweaks that need real code (Arc URL bar, toolbar slide, ...) |
| `src/diatweaks.cpp` | The `diatweaks` command: opens Dia, loads the add-on, edits `tweaks.conf` |
| `src/local_browsing.cpp` | Opt-in native startup fallback and reversible process-local changes |
| `src/dia_native_layout.cpp` | PE/Swift capability resolver; see [Compatibility](docs/COMPATIBILITY.md) |
| `src/engine.h` | What `tap.cpp` and `ui.cpp` share |
| `gen-winrt.cmd` | Makes C++/WinRT headers from the WinUI metadata in your Dia install (or NuGet without Dia) |
| `.github/workflows/release.yml` | Builds and publishes a release when a `v*` tag is pushed |

## Releasing

Everyone on a release gets offered the new one in **Settings → Patches → Updates**, with its notes.

1. On `main`, tag the commit and push the tag:

   ```powershell
   git tag v1.1.0; git push origin v1.1.0
   ```

2. The Release workflow builds `WinDiaTweaks.zip` (against the same WinUI metadata as Dia, from NuGet), and publishes the release with `install.ps1`.
3. The notes list the `feat:` and `fix:` commits since the last tag. To write them yourself, use an annotated tag: `git tag -a v1.1.0 -m "..."` (Markdown works).

Versions follow [SemVer](https://semver.org): new tweaks bump the minor version, fixes the patch version. To rebuild an existing release, run the workflow by hand with its tag.

## Adding a tweak

1. Add an entry to `tweaks.conf`. It shows up in **Settings → Patches** and in `diatweaks list` by itself:

   ```
   [hideFooButton] off
   title: Hide Foo button
   desc: Hides the Foo button in the toolbar.
   section: Toolbar Items
   icon: E8BB
   match name=FooButton => Visibility=Collapsed
   ```

   The header of `tweaks.conf` lists every condition (`name`, `type`, `ancestor`, `upN`, `state=`, ...). A rule's values are written the way WinUI prints them.
2. Find the element to change. With Dia open through `diatweaks`:
   - `diatweaks dump` writes Dia's whole UI tree, with names, types and properties, to `bin\tree.txt`.
   - `diatweaks probe FooButton FooButton*` describes live elements: size, position, visibility, corner radius. A trailing `*` lists everything inside.
3. Hiding, moving or recoloring an element is a rule. Anything that has to react to Dia (animations, moving controls, keyboard shortcuts) goes in `src/ui.cpp`; read `Engine_TweakOn("yourKey")` and undo everything you changed when it turns off.
4. Don't correct Dia on a timer. When Dia sets a property your tweak overrides, the engine hears about it before the frame is drawn and puts your value back, so nothing flickers. Rely on that (rules, or `RegisterPropertyChangedCallback` in code).

## Testing

Native fallback changes must pass `tests\local_browsing_native.cmd`, `tests\dia_native_layout_native.cmd`, and `tests\local_browsing_config.ps1` after building. These test owned child processes, malformed images and an isolated config; they do not use your browser data. The capability test also accepts local `Dia.dll` paths for supported-image and mutation checks. Do not commit those images.

Before a PR, check appearance changes by hand:

- The tweak on, off, and on again without restarting Dia. With it off, Dia must look exactly as it did before.
- **Preset → Arc** and **Preset → None** with your tweak in the mix.
- Sidebar docked, closed, and floating on hover; split view; the New Tab page; switching tabs quickly.
- `diatweaks restart`: the tweak applies cleanly as Dia starts.

`bin\tap.log` records what the engine did.

The forced sign-in bypass requires a restart when changed. Its manual acceptance checks and the distinction between image inspection and runtime verification are in [LOCAL-BROWSING.md](docs/LOCAL-BROWSING.md).

## Commit messages

- Follow [Conventional Commits](https://www.conventionalcommits.org), for example `fix(arc): keep the sidebar button visible in split view`.
- Present tense, lowercase, first line under 72 characters. Only add a body when the change needs explaining.

## Using AI tools

AI assistants are welcome. What you submit is still yours: you need to have run it and checked that it works.

Keep descriptions plain and specific. PRs full of generic, untested generated text will be closed.

## Please don't

- Commit Dia or any of its files. `winmd\` and `gen\` are made from your Dia install, so they stay out of git too.
- Change anything in Dia's install folder, or do anything beyond changing how Dia looks and behaves on screen.
- Read, store or send pages, history, passwords or any other data from Dia.
