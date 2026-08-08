# Git / GitHub notes for this repo

## Which account owns this

`Nicolassalazar-pro/desktop-robot` (private).

**Not** the Microsoft/DXC account. This matters because the environment pushes you
toward the wrong one in two independent ways.

## Two auth traps, and how to get past them

### 1. `GH_TOKEN` overrides your stored login

The agent shell has `GH_TOKEN` set, which authenticates `gh` as
`v-nisalazar_microsoft` regardless of what `hosts.yml` says. Symptom: `gh repo list`
returns 2 repos instead of 26, and this repo appears not to exist.

```powershell
Remove-Item Env:GH_TOKEN, Env:GITHUB_TOKEN -ErrorAction SilentlyContinue
gh api user --jq .login      # must print Nicolassalazar-pro
```

Setting it to `''` is **not** enough — an empty token makes `gh` fail auth outright.
It has to be removed.

### 2. The Copilot app injects a credential helper on git's command line

```
GIT_CONFIG_PARAMETERS = 'credential.https://github.com.helper=' 'credential.https://github.com.helper=copilot'
```

Command-line config outranks every config file, so `.git/config` and `~/.gitconfig`
cannot override it. That helper authenticates as the Microsoft account, so a push to
this private repo fails with a misleading:

```
remote: Repository not found.
```

which reads like the repo is missing rather than like an auth failure.

Fix — clear the injected parameters so the `gh` credential helper in `~/.gitconfig`
applies:

```powershell
Remove-Item Env:GIT_CONFIG_PARAMETERS, Env:GIT_CONFIG_COUNT -ErrorAction SilentlyContinue
```

### Combined preamble for any push from an agent shell

```powershell
Remove-Item Env:GIT_CONFIG_PARAMETERS, Env:GIT_CONFIG_COUNT, Env:GH_TOKEN, Env:GITHUB_TOKEN -ErrorAction SilentlyContinue
git push
```

## What is deliberately not committed

- `.pio/` — resolved libraries and build output. `platformio.ini` is the declaration;
  the resolve is reproducible. It was also the only source of secret-scan hits.
- `*.exe` — host-test binaries rebuilt by `test/run_motor_host_tests.ps1`.
- `*.log`, capture logs — serial evidence from debugging sessions, some hundreds of MB.

## What is committed even though it is large (~101 MB)

`body models/` (STL/3MF of the chassis), `datasheets/` (ST7789V2, QMI8658C, ESP32-S3),
and reference media. These are load-bearing: the ST7789V2 datasheet is what settled the
SPI clock question, and the STLs are the physical robot. They are binary and do not
diff, but they belong with the project.

## Credentials in the source

`kOverridePassword = "TheGarageAtl"` is in `main.cpp`. It is the robot's own softAP
password and is displayed on the LCD as a QR code by design, so it is not a secret. The
repo is private regardless. A secret scan before the first push was otherwise clean.
