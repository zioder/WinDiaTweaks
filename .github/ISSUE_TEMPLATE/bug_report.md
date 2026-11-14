---
name: Bug report
about: Something broke, crashed or looks wrong with the tweaks on
title: ""
labels: bug
---

## What happened

<!-- What you did, what you expected, what happened instead. Screenshots or a short video help a lot. -->

## Steps to reproduce

1.
2.
3.

Does it also happen when Dia is opened from the Start menu (without the tweaks)? Yes / No

## Versions

- Dia: <!-- Settings > About, e.g. 1.6.1.3 -->
- WinDiaTweaks: <!-- Settings > Patches > Updates, or `diatweaks version` -->
- Windows: <!-- Win + R, `winver`, e.g. 11 24H2 build 26100.4061 -->

## Logs

Please attach these files from the WinDiaTweaks folder: `%USERPROFILE%\WinDiaTweaks\bin` for an installed release, or the `bin` folder of your checkout if you built it yourself. Paste the folder path into File Explorer's address bar to open it.

- [ ] `tap.log`: what the tweaks did while Dia ran (also `tap.log.prev`, from the session before, if the problem was in an earlier session)
- [ ] `crash.log`: only there if Dia crashed: the error, the crashing stack and which tweaks were on (also `crash.log.prev` if there is one)
- [ ] Output of `diatweaks status`: run it in the folder above and paste it below. It lists which tweaks are on.

```
paste `diatweaks status` here
```

If Dia crashed, please give the time it happened (the logs are timestamped).

<!-- The logs hold the tweaks' own messages: names of Dia's UI elements, tweak settings and timings, and
     sometimes a site address (Peek, Ctrl+T). Have a quick look before attaching and remove anything you'd
     rather not share. -->
