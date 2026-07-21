# Third-party notice — Interception

`interception.c` and `interception.h` in this directory are **not our code**.
They are the client library of the Interception API by Francisco Lopes
(oblitum).

- Upstream: https://github.com/oblitum/Interception
- Author contact: francisco@oblita.com

## Licensing — read before shipping

Interception is **dual-licensed**, and the split is on the *purpose of use*,
not on distribution.

### Non-commercial use

**LGPL 3.0**, upstream `licenses/non-commercial-usage/LGPL 3.0.txt`.

Upstream README, verbatim:

> For non-commercial purposes it adopts LGPL for the library and its source
> code, with rights of distribution of the related binary assets (drivers and
> installers) once communication with drivers happen solely by use of the
> library and its API.

Note the condition attached to redistributing the driver/installer binaries:
communication with the driver must happen *solely through the library API*.
The `interception_input` wrapper satisfies this — it calls only the public
Interception API and never talks to the driver directly.

### Commercial use

Requires a **paid license** from the author. Upstream publishes two, as PDFs
under `licenses/commercial-usage/`:

- **Interception API License** — the non-commercial terms with the commercial
  restriction removed, plus an installer library for silent driver install.
- **Interception License** — full source access, including drivers and
  installers.

> Please contact me at francisco@oblita.com for acquiring a commercial license.

## Why this matters here

This restriction is on **use**, not conveying. SwitchDesk's GPL posture rests
on hosting rather than distributing — that argument does **not** transfer.
Running Interception on a production node to serve a paying customer is
commercial use even though no binary is ever distributed.

Practical consequence:

- Bench and development testing on a personal/non-commercial node: covered by
  the LGPL grant.
- Any paid customer session: requires the commercial license first.

## Status in this repo

The vendored copy originally arrived with no license file and no attribution
header. This notice restores the attribution. The LGPL 3.0 text itself is not
duplicated here; it is at the upstream path above and is the standard
GNU LGPL 3.0.

Retrieved from upstream 2026-07-21.
