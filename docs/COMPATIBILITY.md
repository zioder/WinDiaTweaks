# Compatibility

WinDiaTweaks targets Windows 11 x64 and the packaged Windows version of Dia. Compatibility follows the interfaces and capabilities a build exposes, rather than an exact Dia version list. A version listed as tested is evidence about that build, not a requirement that other builds have the same version.

## Appearance tweaks

The add-on uses published WinUI/XAML diagnostics interfaces. Build headers come from Dia's installed WinUI metadata, or compatible NuGet metadata on machines without Dia. Individual tweaks find the UI elements they need at runtime. Dia can change those elements, so a later release may require an adjustment even when the interfaces remain compatible.

## Native forced sign-in bypass

The opt-in native fallback inspects each installed `Dia.dll` before starting Dia. It discovers the required Swift types, fields, enum cases and native control flow, then validates their relationships. A missing or ambiguous capability disables this fallback with an error before an existing browser is closed. It never guesses an offset from a nearby release.

Native admission must not depend on an allowed version, file hash, timestamp or fixed module offset. Exact instruction checks are made at the sites discovered in that image. Runtime metadata and loaded bytes must agree with the inspected layout before any change is applied. A measured SHA-256 ties the inspected file to the file actually loaded; it is a freshness check, not a release allowlist.

Changes stay in the launched process. Updating Dia preserves the saved toggle, and the next tweaked launch resolves the new image again. A structurally incompatible update may need a WinDiaTweaks change before this optional fallback works. See [LOCAL-BROWSING.md](LOCAL-BROWSING.md) for the runtime behavior and verification limits.

## Verification

Report manual browsing tests separately from static compatibility checks and owned-child tests. Resolving an update's image does not establish that the update has been run successfully. Keep vendor binaries out of the repository; tests can accept local image paths for additional compatibility checks.
