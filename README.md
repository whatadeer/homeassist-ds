# homeassist-ds

A homebrew 3DS remote for Home Assistant. Bottom screen lists lights,
switches, fans, locks, covers, input_booleans, climate and media_player
entities pulled from `GET /api/states`; touching one calls
`homeassistant.toggle` on it. Top screen shows connection status.

## 1. Get a toolchain

### Option A: container (recommended, nothing installed locally)

[Containerfile](Containerfile) builds on the official `devkitpro/devkitarm`
image plus the portlibs this project needs (citro2d/citro3d, curl, mbedtls,
jansson, zlib). All you need on the host is Podman.

```powershell
.\build.ps1          # make cia   -> ha3ds.cia
.\build.ps1 3dsx     # make       -> build\ha3ds.3dsx
.\build.ps1 clean
```

The first run builds the image (a few minutes); later runs reuse it. Output
files land directly in the project folder via the bind mount.

Equivalent raw commands, if you'd rather not use the script:

```
podman build -t ha3ds-builder -f Containerfile .
podman run --rm -v ${PWD}:/project ha3ds-builder make cia
```

(On native Linux with SELinux enforcing, add `:Z` to the volume mount:
`-v ${PWD}:/project:Z`.)

### Option B: native devkitPro install

Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) (its
installer sets `DEVKITPRO`/`DEVKITARM` for you), then install the packages
this project needs:

```
(dkp-)pacman -S 3ds-dev general-tools 3ds-citro2d 3ds-citro3d \
                 3ds-curl 3ds-mbedtls 3ds-jansson 3ds-zlib
```

- `3ds-dev` — devkitARM + libctru
- `general-tools` — `makerom` + `bannertool`, needed to produce the `.cia`
- `3ds-citro2d` / `3ds-citro3d` — the 2D rendering used for the UI
- `3ds-curl` + `3ds-mbedtls` — HTTP(S) client (mbedtls is curl's TLS backend
  for HTTPS instances)
- `3ds-jansson` — JSON parsing for the HA REST API responses

## 2. Build

If you went with Option A (container), you already built it in step 1 -
`.\build.ps1` produces `ha3ds.cia` in the project folder.

If you went with Option B (native devkitPro):

```
make          # build/ha3ds.3dsx  - run via Homebrew Launcher, or in Citra
make cia      # ha3ds.cia         - sideload via FBI on CFW (Luma3DS)
```

Copy `ha3ds.cia` to your SD card and install it with
[FBI](https://github.com/Steveice10/FBI).

## 3. Sign in (on the 3DS)

No build-time configuration needed. On first launch the app walks you
through sign-in on the console itself:

1. Enter your Home Assistant URL (a bare hostname gets `https://`
   prepended). Plain-HTTP LAN addresses like `http://192.168.1.50:8123`
   work too.
2. Enter your HA username and password (and a two-factor code if your
   account uses MFA) on the system keyboard.

The app drives Home Assistant's own login flow (`/auth/login_flow` +
`/auth/token` — the same one the web UI uses) and stores only the
resulting **refresh token** at `sdmc:/ha3ds.cfg`; your password is never
stored. Short-lived access tokens are minted from it automatically at
runtime. The session shows up in your HA profile's "Refresh Tokens" list,
named after your instance URL, and can be revoked there at any time.
Press **SELECT** in the app to sign in again (different server or
account).

**HTTPS note:** requests are verified against `data/cacert.bin` - curl's
official Mozilla CA bundle, embedded into the binary at build time (see
"CA bundle" below). A self-signed cert will be rejected outright; there's
no option to bypass verification, since credentials are worth protecting
from anyone on-path. If cert validation fails unexpectedly on a cert
that's otherwise fine, check the 3DS's system clock/date first -
expired-looking certs from a wrong clock are a common false alarm.

## Notes / things worth knowing

- Networking uses `soc:U` (BSD sockets over Wi-Fi) via libcurl, not the
  system HTTPC service — the 3DS needs to already be connected to your Wi-Fi
  network and Home Assistant needs to be reachable on that network.
- `data/cacert.bin` is curl's official Mozilla CA bundle
  ([curl.se/ca/cacert.pem](https://curl.se/ca/cacert.pem)), embedded into
  the binary via the Makefile's `DATA`/`bin2s` pipeline and used for HTTPS
  cert verification (`source/ha_client.c`, `CURLOPT_CAINFO_BLOB`) — the 3DS
  has no system trust store libcurl can fall back on. It's just a text file
  of root certs; `.github/workflows/update-cacert.yml` re-downloads it
  monthly and opens a PR if it changed, so CA rotations don't need to be
  tracked by hand.
- `resources/app.rsf` is the makerom packaging spec (title/permissions).
  It's adapted from a known-working homebrew CIA project
  ([FBI](https://github.com/Steveice10/FBI)'s build config) — if `makerom`
  ever complains about a field, that's the part to check first, not your C
  code.
- `meta/icon.png` and `meta/banner.png` are Home Assistant's own logo
  (sourced from [home-assistant/android](https://github.com/home-assistant/android)'s
  app icon), resized to bannertool's required 48x48 / 256x128. See
  `meta/gen_placeholder_assets.py`'s docstring for how to regenerate them
  from a newer logo. `meta/audio.wav` (silent CIA banner audio) is still
  generated by that script.
- The entity list is capped at 64 entities and filtered to toggleable
  domains to keep it usable on a small screen. Which domains are pulled is
  a runtime setting (in-app Settings screen, START from the main list, backed
  by `HA_DOMAINS`/`ha_client_set_enabled_domains()` in `source/ha_client.c`);
  adjust `MAX_ENTITIES` in `source/main.c` if you want more entities overall.
