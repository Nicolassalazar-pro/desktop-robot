# Maintainer Git notes

The public repository is:

`Nicolassalazar-pro/desktop-robot`

## Multi-account shells

Before a release or visibility change, verify which GitHub identity the shell
will use:

```powershell
gh auth status
gh repo view Nicolassalazar-pro/desktop-robot
```

Environment variables such as `GH_TOKEN` and injected credential helpers can
override the account selected in the GitHub CLI keyring. For a one-off push,
prefer an explicit token from the intended stored account rather than deleting
or replacing machine-wide credentials.

## What is deliberately not committed

- `.pio/` - resolved libraries and build output
- `build/` - generated preview and firmware artifacts
- `*.exe` - host-test binaries rebuilt by the test runner
- `*.log` and serial captures - local diagnostic evidence, often very large

## Large files that are intentionally tracked

`body models/`, selected datasheets, and reference media are part of the
reproducible physical project. They do not diff usefully, but they are
load-bearing design inputs rather than disposable build output.

## Public credential model

The softAP passphrase is visible in source and on the LCD pairing QR by design.
It is not the actuation credential. HTTP and WebSocket access require a
separate random per-activation token shown locally on the display. The threat
model and known residual risks are documented in
[`key-issues/SEC.md`](key-issues/SEC.md).

Run a history-level secret scan before publishing rewritten history or adding
new binary archives.
