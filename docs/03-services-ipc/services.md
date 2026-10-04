# System services

Condensed from the Switchbrew wiki. Collected 2026-10-04 — re-check the source pages, they change often.


## Services

Source: <https://switchbrew.org/wiki/Services_API>

Core: `sm:` service manager, `fsp-srv` filesystem, `hid` input, `nvdrv` GPU
driver, `vi` display, `appletOE/AE` applets, `set`/`set:sys` settings, `pm:*`
process management, `ldr:*` loader, `lm` logging, `time:*`, `acc:*` accounts,
`audout/audren` audio, `bsd/nifm/ssl` network, `spl:` crypto, `es` tickets,
`nfc/nfp` NFC. Switch 2 adds/changes services; check the Switch 2 title list.
Suggested stub order for a first homebrew: `sm:`, `fsp-srv`, `set`, `time`,
`hid`, `vi`, `nvdrv`, `appletOE`, `lm`.
