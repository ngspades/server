# Security

A game server is on the internet: anyone can send it packets, chat lines and
queries without an account. We take a report seriously and answer it.

## Reporting a vulnerability

**Do not open a public issue.** Use GitHub's private reporting instead:
the repository's **Security** tab, **Report a vulnerability**
(<https://github.com/ngspades/server/security/advisories/new>).

Please include:

- what an attacker can do (crash the server, read or write memory, run
  commands, ban or kick others, amplify traffic, bypass a password...);
- who can do it (anyone on the internet, any player, a logged-in role, the
  local console);
- the steps or a packet capture to reproduce it, and the version (the commit
  or the release tag);
- if you have one, an AddressSanitizer report (`make server-asan`).

We acknowledge a report within a week, tell you what we found, and credit
you in the fix's release notes unless you ask us not to. Please give us the
time to release a fix before you publish.

## What is in scope

- The server binary and everything it parses: protocol 1.0 and 0.75
  packets, chat and commands, the UDP queries (`HELLO*`), WebRTC frames,
  the master server's answers, map files and their
  `.txt` and `.toml`, `config.toml`, plugins' API boundaries.
- `common/`: the protocol codec, the map formats.
- The release archives and the Docker image.

Out of scope: a plugin's own code (a server owner runs the plugins they
choose), a denial of service by sheer volume that the network layer's own
limits already shape (see `docs/hosting.md`), and default passwords left
unchanged (the server warns about them at every start).

## Supported versions

Fixes go into `main` and the next release. Run the latest release.

## Hardening already in place

Per-player budgets for packets, chat and commands (`Flood.c`); UDP answers
never larger than their query (no amplification); a cap on unfinished
handshakes; connect tokens from the master for internet joins; every map
access bounds-checked (`MapStore.h`); protocol 1.0's codec fuzzed in its
tests; CI builds and plays every game mode under AddressSanitizer and
UndefinedBehaviorSanitizer.
