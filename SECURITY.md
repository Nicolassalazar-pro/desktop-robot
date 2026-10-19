# Security policy

Desktop Robot controls physical movement. Please report vulnerabilities
privately rather than opening a public issue.

## Report a vulnerability

Use the repository's
[private security advisory form](https://github.com/Nicolassalazar-pro/desktop-robot/security/advisories/new).

Include:

- affected commit or firmware version
- attack preconditions (USB access, softAP access, URL token, physical access)
- reproduction steps
- expected physical impact
- whether the 300 ms drive lease or hardware timer stopped motion

## Supported version

Security fixes target the latest commit on `main`.

## Threat model

The phone-control flow creates a temporary WPA2 softAP. HTTP and WebSocket
access are independently gated by a random per-activation token shown on the
LCD. Seeing the screen is the intended physical-presence authorization
boundary. The full current analysis and known residual risks are tracked in
[`docs/key-issues/SEC.md`](docs/key-issues/SEC.md).
