# Security

FrameDepth runs on your headset: a Chrome extension and a companion program
that listens on `127.0.0.1` only and accepts the extension's Origin only. It
sends nothing off the device.

## Reporting a problem

Please report security problems privately, not in a public issue: use
**Report a vulnerability** on the repository's Security tab
(<https://github.com/guix8/framedepth/security/advisories/new>). Say what is
affected, how to reproduce it and what an attacker could do with it.

Only the latest release is supported; fixes go into the next release.

## Scope

In scope: the extension (`extension/`), the companion (`companion/`) and the
install and release scripts (`packaging/`, `scripts/`). Known and documented
limits (a page can tell the extension is installed; another program of the
same user can connect to the companion) are listed in
[docs/how-it-works.md](docs/how-it-works.md#limits). Problems in Chrome,
SteamOS, SteamVR or the bundled third-party components belong to their own
projects.
