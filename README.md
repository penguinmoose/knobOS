# KnobOS
**Current version: 10.6**

A handheld device project with a rotary knob, neopixel ring backlighting, and 128x64 monochrome display with three buttons. It runs a mini-app operating system called KnobOS.

It has two functional miniapps: Speaker control, and Mixer control. Using the Mixer miniapp, it can control a **Midas M32R** mixing
console over OSC. With the Speaker miniapp, it can control the volume of a Sony soundbar over Sony's local HTTP API, and
**Spotify** over the Web API all in one app, displaying the live song progress and details too!

The system also includes a nav stack, a settings miniapp, persistent
configuration via NVS, battery monitoring, and sleep.

<!-- Screenshot / photo strip goes here -->

---

## Hardware

> *This section is a summary. Precise wiring, part numbers and photos are
> maintained above/below by hand — expand as needed.*

| Part | Notes |
|---|---|
| **Unexpected Maker FeatherS2** | ESP32-S2, 8MB PSRAM. **PSRAM must be enabled** (Spotify does not work without it) |
| **Adafruit 128×64 OLED FeatherWing** | SH1107 at I²C `0x3C`, mounted upside down (rotation 3). Carries buttons A/B/C. |
| **EC11 / KY-040 rotary encoder** | Quadrature + shaft button. |
| **12-pixel NeoPixel ring** | Pixel 0 at 6 o'clock, advancing clockwise. V+ to **BAT, not 3V3**. |
| **21700 Li-ion** | On the FeatherS2's battery port in parallel with NeoPixel power. |

### Pin map

| Function | GPIO | Notes |
|---|---|---|
| Encoder CLK (A) | 18 | |
| Encoder DT (B) | 17 | |
| Encoder SW | 0 | BOOT strapping pin — holding the shaft during reset enters the ROM bootloader |
| Button A (bottom) | 1 | OLED FeatherWing |
| Button B (middle) | 38 | OLED FeatherWing |
| Button C (top) | 33 | OLED FeatherWing |
| I²C SDA / SCL | 8 / 9 | UM variant pinout — **not** the Adafruit ESP32-S2 Feather's |
| NeoPixel data | 7 | |
| VBAT sense | 3 | ADC1_CH2, **external divider required** |
| 5V present (from external USB port) | 10 | 5.1k:10k divider, RTC-capable so it can wake from deep sleep |

Hardware notes:
- **The plain FeatherS2 has no on-board battery divider.** The *Neo* has one on
  IO2 and the FeatherS3 on GPIO2; the original board does not. Two equal
  resistors (e.g. 2× 100k) from BAT to GPIO 3 are required. The ratio and both
  voltage endpoints are configurable in Settings.
- **A 3.3V data line drives WS2812s fine at battery voltage.** Their logic-high
  threshold is 0.7 × VDD, ≈2.6V at 3.7V. At 5V it would be 3.5V, which is why
  level shifters are usually needed and aren't here.
- Connect EC11 `+` to **3V3, never 5V** because the S2 is not 5V tolerant.

---

## Building

Arduino IDE or `arduino-cli`, board **UM FeatherS2** (`esp32:esp32:um_feathers2`).

Two board-menu settings are important:

| Setting | Value | Why |
|---|---|---|
| **USB CDC On Boot** | **Enabled** | Native USB. Without it `Serial` goes to the physical TX/RX pins and you get no output at all. Baud rate is irrelevant over CDC. |
| **PSRAM** | **Enabled** | 8MB. Used for the Spotify response buffer *and* for mbedTLS's TLS record buffers. With PSRAM off, HTTPS handshakes fail. |

Libraries: Adafruit GFX, Adafruit SH110X, Adafruit BusIO, Adafruit NeoPixel.

```bash
arduino-cli compile --fqbn esp32:esp32:um_feathers2:PSRAM=enabled knob_os.ino
```

Current footprint: 56% flash, 26% static RAM.

---

## First run

Nothing is baked into the firmware — no SSIDs, no keys, no IPs. Everything is
entered on the device and stored in NVS.

1. **Wi-Fi** — Settings → Wi-Fi → Scan, pick a network, type the password with
   the on-screen text entry. Saved networks are remembered and reconnected.
2. **Mixer** — Settings → Mixer → IP, or turn on Demo Mode to drive the real
   UI from a simulated console with no hardware present. **The console address
   is stored per Wi-Fi network** — see below.
3. **Speaker** — Settings → Speaker → Scan (SSDP discovery), or Manual IP.
4. **Spotify** — see [SPOTIFY_SETUP.md](SPOTIFY_SETUP.md). Five minutes, needs
   a Spotify **Premium** account and a free developer app. You never type the
   long tokens on the device: the knob serves a small config page over your LAN
   and you paste into that from a browser.

Speaker Control works **without** a speaker (as a pure Spotify remote) and
**without** Spotify (as a pure volume knob). Neither is a prerequisite for the
other.

### The console address is per network

A mixing console lives on one network. A single global address meant carrying
a venue's IP onto the home network, where it points at nothing but still reads
as configured — so the mixer app would open, draw a working fader, and send
OSC into the void.

Addresses are therefore keyed by SSID, and the live one is adopted whenever
the network changes. On a network with no address the link stays off the wire
entirely and the mixer behaves exactly as it does when nothing has ever been
set up: straight to the IP screen, with the Demo button there.

**Settings → Mixer → Networks...** lists every network the device knows about
with the address for each. The list is assembled from the network you are on,
the saved networks, and any stored address belonging to neither — so an
address outlives its network being forgotten and can still be cleared.

> **Upgrading from 9.x:** the single address you had is migrated at boot onto
> the most recently *saved* network, because which one it belonged to was
> never recorded. Check **Settings → Mixer → Networks...** and move it if the
> guess was wrong.

---

## Using it

**Home** is a three-tile carousel: Mixer, Speaker, Settings. Turn to select,
click to enter.

**Back is A+B** — the bottom and middle buttons together. Only the two chord
members defer (by a configurable ~150 ms window); every other button fires
immediately, so navigation stays snappy.

### Speaker Control

| Input | Does |
|---|---|
| Turn | Volume, or the playhead when there is no speaker (see below) |
| Top button (C) | Next track |
| Bottom button (A) | Previous track |
| **Hold** C or A | Cue and review: scrub forward/back, with configurable hold time, speed, granularity and acceleration |
| Middle button (B) | Play/pause, or mute (`Centre Btn` picks; `Auto` means play/pause when Spotify is authorised) |
| **Hold** the knob + turn | Seek, at the `Hold Seek` rate, even while a speaker has the knob |

The screen shows title, artist, a progress bar and both clocks. The progress
clock runs locally between polls, so it moves smoothly at 20fps and freezes the
instant you pause rather than waiting for the cloud to confirm.

**With no speaker, the knob seeks instead.** A volume bar with nothing behind
it is a control that does nothing, so it goes, and the progress block takes the
bottom third of the screen at twice the bar height. The artist moves down a
line and the row under the title goes to the album, or to a second line of
title. One detent moves the
playhead by Knob Rate A, or Knob Rate B while the shaft button is held (the
same two-rate arrangement as the mixer fader, and B may be the larger of the
two). Acceleration is opt-in per rate, so spinning fast covers more ground.

The sweep is local and only the resting position is sent, 400 ms after the knob
stops. A Spotify seek is a round trip of a few hundred milliseconds, so one per
detent would flood the API and lag behind the knob. Seeking does not interrupt
playback, so the track keeps running while you scrub.

With a speaker connected the volume has the knob, so seeking is asked for:
hold the shaft button and turn. That rate is separate (`Hold Seek`, 15s per
detent by default) rather than a reuse of Rate B, because it is the only seek
rate available in that mode and wants to sit between a fine and a coarse one.
Setting it to `Off` gives the shaft button back to play/pause alone.

The shaft button is a modifier as well as a button, so its click is decided on
release: a hold that moved the knob was a seek, not a play/pause. Mute (if you
have the shaft set to hold-for-mute) checks the same thing, since a hold that
seeks is not a request to silence the room.

### Title, or album

The row under the title is contested. Settings → Speaker → **Title** decides:

| Mode | Does |
|---|---|
| `Show album` | Album always gets the row; a long title marquees on one line |
| `Wrap song` | Title takes the row when it needs a second line |
| `Automatic` | Wraps only when the title needs it, shows the album otherwise |

Wrapping breaks on the last space that still fits the first row, which makes
the second row as short as possible and so as likely as possible to fit. A
single long word breaks mid-word rather than refusing to wrap.

**If Too Long** covers a title that will not fit even two rows: `One line`
collapses to a single marquee and gives the row back to the album, `Two lines`
wraps anyway and lets the second row scroll. Because the split fills the first
row first, it is usually the second row alone that moves.

The artist stays on the third row in every case. Letting it move would make the
layout jump between tracks, which is a worse cost than an occasional empty row.

**The soundbar address is stored per network**, like the console address, so
on a network with no speaker on it the app is in Spotify mode from the first
frame rather than spending six seconds proving there is nothing to talk to.
Settings → Speaker → **Networks...** reaches the same list as the mixer's;
each row holds both addresses for that network.

Settings → Speaker → **Mode** picks which half leads:

| Mode | Does |
|---|---|
| `Speaker default` | Presumes a speaker is there and falls back to Spotify if none answers |
| `Spotify default` | Starts on the playhead and switches to volume once a speaker answers |
| `Spotify only` | Never puts the speaker on the wire at all |

All three end up in the same place once the facts are in; they differ only in
what is presumed while the first poll is still in flight. A speaker that is
switched off counts as absent, since the volume knob does nothing either way.
Once one has answered, losing it takes six seconds of silence, so a dropped
packet does not rebuild the layout.

### Mixer Control

8 groups, 78 strips. Fader plus a live meter with peak hold and zone-boundary
ticks. Turn to move the fader; the shaft button switches to a second,
independent rate — which may be *larger* than the first, making hold a coarse
mode rather than a fine one. Acceleration is opt-in per rate.

With no Wi-Fi, entering the mixer offers **Demo Mode...** on the Wi-Fi screen
itself: with no network there is no console either, so the simulator is the
only thing it can actually do from there.

**Demo Mode** drives the whole real UI from a local console model, intercepted
at the transport layer. No UI code has demo branches.

---

## Software architecture

### Task model — the most important thing to understand

Three execution contexts:

```
loop()      UI: encoder, buttons, display, app ticks, mixer UDP
netTask     LAN work: Sony soundbar HTTP        (6KB stack)
syTask      Cloud work: Spotify HTTPS/TLS      (16KB stack)
```

**Why two network tasks.** A TLS handshake genuinely needs several seconds on
this chip. When Spotify and the speaker shared one serial task, any timeout
long enough for Spotify stalled the speaker's volume writes behind it.
Shortening the timeout fixed the speaker and broke Spotify; lengthening it did
the reverse. They are different workloads — one LAN-fast, one cloud-slow — and
needed separate tasks, not a compromise timeout suiting neither.

Each has its own **recursive** mutex. They must be recursive: the 401-retry
path re-enters via the token refresh, and a plain FreeRTOS mutex self-deadlocks
there.

**The UI never blocks on the network.** Apps set flags; the tasks act on them.
Queued commands are checked before polls, and polls are selected **round-robin**
from a rotating cursor rather than by fixed priority — a strict chain let a
slow, frequently-scheduled poll crowd out the one below it forever.

### App model

Every screen is an `App`: a name, an icon, and a set of optional handlers.

```c
struct App {
  const char *name;
  void (*drawIcon)(int x, int y, int s);
  void (*onEnter)(); void (*onExit)();
  void (*tick)(); void (*draw)();
  void (*onButton)(BtnId, BtnEv);
  void (*onKnob)(int steps);
  void (*launch)();      // prerequisite gate, used instead of a push
  bool (*onBack)();      // true if the app consumed the back gesture
};
```

A nav stack pushes and pops them. All handlers are null-checked, so an app
implements only what it needs. There are 33 registered, from `AppHome` down to
`AppChargeWait`. Nav position survives sleep and reboot via NVS.

**Detent ownership:** `loop()` consumes `knobSteps()` only for apps that declare
`onKnob`. Apps that read the encoder in their own `tick` leave it null, or the
two compete.

### Encoder

Interrupt-driven quadrature decode on **both edges of both channels** through a
16-entry state table. Illegal transitions map to zero, so bounce is rejected by
the decoder rather than by a debounce delay — no detent is missed however fast
you spin it.

The encoder is **relative**, and that is the point. This project began with a
motorised knob on a servo, whose absolute position constantly had to be
reconciled with a remote value. A relative encoder has nothing to reconcile: a
remote change overwrites the value and the next detent continues from there.
That deleted an entire bug class that had dominated two major versions.

### Display

20fps, full redraw each frame. `CONTENT_W` is mutable and reset to narrow every
frame; screens wanting the full panel call `uiWide()` from their `draw()`.

| | |
|---|---|
| Content (narrow / wide) | 0–109 / 0–125 |
| Divider | x=112 |
| Button glyph column | 117–125, centred at 121 |

### Settings

Declarative `CfgItem` rows (int, bool, enum, checkbox, action, link) in
`CfgPage` tables, rendered by one generic page renderer. Persistence is NVS
with debounced writes (~1.5 s), ~80 keys.

Three things the row type carries beyond a value:

- **`C_CHECK`** toggles on select instead of opening an edit mode. Dropping
  into one to change a two-state value you can already see buys nothing, and a
  two-option dropdown would misrepresent independent options as alternatives.
- **`visible()`** hides a row entirely. A setting that only means something in
  one mode is absent in the others rather than sitting there doing nothing —
  `sel` counts *visible* rows, and every access goes through `cfgItemAt()`.
- **`dynLabel()`** renames a row at runtime, for one that opens different
  things at different times and should say which.

Every page repeats its row count as a literal, and forgetting to bump it on
adding a row silently hides that row — a mistake this project made more than
once. Each page declaration is now followed by a `static_assert` tying the
literal to its array, so the next one is a build error instead.

Buttons have a **Stuck Cut**: a button held longer than a threshold (default
10 s) is treated as pressure rather than intent and ignored entirely until
released — a device pressed against something in a bag can't repeat its way
through a settings page.

### NeoPixel ring

A gauge, not decoration. Brightness is applied per pixel in software rather
than through `setBrightness()`, because the clip indicator has to be able to
ignore the master level entirely — a clip you might miss is not a clip
indicator.

- **Smoothing** lights the pixel straddling the level proportionally, so twelve
  pixels read as continuous. Same idea as anti-aliasing a line.
- **Meter mode** colours each pixel by *its own* position on the scale, not by
  the current level. That is what makes it read like a console meter rather
  than a bar that changes colour.
- **Auto mode** picks fader or meter *per frame* rather than making it a mode
  you have to remember to switch. Two independent conditions under
  Settings → NeoPixel → Mixer → Auto mode: show the fader just after a turn
  (for a configurable Fader Hold), and show the fader while the channel is
  quiet (after a configurable Quiet Delay). Independent rather than a choice,
  because wanting the fader while adjusting and wanting it on a silent channel
  are unrelated reasons and any combination is sensible.
  Quiet Delay exists because music has gaps in it: a beat between phrases, a
  fade, the space between tracks. Without it every one of those snapped the
  ring to the fader for a frame. A strip with no meter at all (MAIN, DCAs) is
  exempt from the delay, since that is not silence but the absence of a meter,
  and waiting will not change it.
- The **speaker ring** follows the knob: volume when the knob is on volume,
  playhead when it is on the playhead, with a separate colour while the second
  seek rate is held. `Progress` and `Volume` force one or the other.
- RGB/HSV colour editor with live preview; origin and reverse settings so the
  ring can be mounted any way round without rewiring.

### Battery and sleep

SoC with a rolling average plus a separate display latch (the average smooths
ADC noise; the latch stops the number twitching across a boundary). Charging
bolt, warnings at 20% and 10%.

**Light sleep is the default**: all four buttons wake it and the screen resumes
exactly where it was. Deep sleep is offered for storage but can only wake on
the shaft button — GPIO 33 and 38 are outside the S2's RTC range (0–21) and
physically cannot wake the chip.

Both wake sources are *level* triggered, so a button still held when sleep
begins is not a press waiting to happen — it is the wake condition already
true. Sleep therefore waits for every armed button to come up first.

Flat-battery shutdown wakes on the **charger line**, not a timer. Polling to
re-check the level would spend energy from an already flat pack and could not
change the outcome; an RTC pin costs nothing while asleep and fires exactly
when something can be done.

---

## Devices and protocols

### Midas M32R — UDP OSC, port 10023

`/xremote` resubscribe every ~4 s. Fader is 0..1 with a piecewise taper
(0.75 = 0 dB). `/meters/0` is 70 little-endian floats: `[0..31]` channels,
`[32..39]` aux, `[40..47]` fx, `[48..63]` bus, `[64..69]` matrix. MAIN and DCAs
have no meters, and DCAs use `/fader` rather than `/mix/fader`. X-Air variants
report meters as int16 dBFS×256 — there's a model setting.

Bidirectional sync is handled by an explicit `MX_IDLE / MX_KNOB / MX_MIXER`
arbiter. Echo windows and confirmation counts always leave a gap somewhere;
one owner at a time does not.

### Sony HT-NT5 — HTTP JSON, `http://<ip>:10000/sony/<service>`

No auth. Volume is instant because it never leaves the LAN. Note that
`audio.setAudioVolume` takes the volume as a **string**, not an int.

**The architectural gap that shapes this whole project:** the soundbar never
models the Spotify Connect stream. With music playing it still reports
`state: STOPPED`, no title, no artist, no position — it knows only that the
active source is `netService:audio?service=spotify`. So its local transport
verbs have nothing to act on. That is a limitation of the soundbar's firmware,
not a parameter error, and it is the entire reason the Spotify Web API
integration exists. Local transport *does* work for USB, Bluetooth and DLNA
sources, and there's a setting to use it.

### Spotify Web API — HTTPS

OAuth2 with the refresh token in NVS; requires Premium. A few things that cost
real time to discover:

- The token endpoint needs **HTTP Basic** credentials, not body params.
- Poll `/v1/me/player?additional_types=track&market=from_token` — the `market`
  parameter drops two ~180-entry `available_markets` arrays that would
  otherwise fill the buffer before the fields you want.
- **Spotify pretty-prints its JSON** (`"key" : value`, spaces around the colon).
  Parsers matching `"key":` silently find nothing. The `cj*` scanners here are
  whitespace-tolerant and allocation-free, scanning a static buffer.
- Keys are ordered alphabetically, so within `item` the sequence runs album,
  artists, …, duration_ms, …, name. Anchoring the parse on `duration_ms` yields
  the *track* name, and the last `artists` before it is the track's list rather
  than the album's. This is the fragile part.
- A 404 on a transport call is `NO_ACTIVE_DEVICE`, not a failure.

#### TLS memory — read this before touching anything Spotify

The Arduino ESP32 core's shipped `sdkconfig` says:

```
CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y        # mbedTLS pinned to internal RAM
CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384
# CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN is not set   # so BOTH buffers are full size
```

Which means mbedTLS **never calls the general allocator**, and a handshake wants
~33KB of internal RAM in two ~16.5KB pieces plus the certificate chain.
`ESP.getMaxAllocHeap()` is `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)`
— the exact same pool — and it reads 32–38KB on this board. That boundary was
the cause of a long-running, intermittent `connect -1` failure.

The fix (v9.9) is `mbedtls_platform_set_calloc_free()` at boot, sending mbedTLS
allocations ≥1KB to PSRAM with an internal fallback: the runtime equivalent of
`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, which a stock Arduino install cannot set.
It is safe because mbedTLS record buffers are filled by lwIP copies, never DMA,
and the hardware AES driver bounces external-RAM buffers through internal DMA
memory.

The device does **not** verify Spotify's certificate (`setInsecure()`). Traffic
is encrypted; server identity is not checked. Pinning a root CA would be
stronger but bricks the device whenever Spotify rotates certificates.

---

## Troubleshooting

Everything diagnostic is on the device, because the S2 talks over native USB
CDC and serial output is unavailable unless USB CDC On Boot is enabled.

**Settings → Speaker → Speaker info** is the main page: link state, raw API
responses, and a Spotify block with poll count, last HTTP code, body length,
RSSI, PSRAM size, largest free block, which heap TLS is drawing from
(`tlspool`), and the last error.

**Settings → NeoPixel → Test** writes the ring directly, with no reference to
the enable flag, the per-app modes, the rate limiter or the dirty check. That
separates a driver or wiring fault from app logic not calling the ring, which
cannot be told apart from the outside. It also reports the library's pixel
buffer pointer — `buf NULL` means its allocation failed.

### Reading a Spotify connection failure

`HTTPClient` collapses a refused socket, a timeout and an out-of-memory
handshake into the same `-1`. Since v9.9 the real mbedTLS code is read back and
shown:

| On screen | Means |
|---|---|
| `tls -0x7F00` | `SSL_ALLOC_FAILED` — out of memory in whichever pool |
| `tls -0x7780` | handshake failure alert from the far end (cipher/protocol) |
| `no socket` | TCP never opened — TLS was never reached at all |
| `DNS fail` | name resolution, checked explicitly before the connect |
| `heap blk N` | refused before starting; only possible with PSRAM disabled |

If Speaker info says `tlspool internal`, PSRAM is off in the board menu.

---

## Repository layout

| File | Purpose |
|---|---|
| `knob_os.ino` | The firmware. Everything. |
| `CHANGELOG.md` | Full per-version history |
| `KNOB_OS_HANDOFF.md` | Engineering handoff: architecture, hardware detail, and a debugging-history section of lessons worth keeping |
| `SPOTIFY_SETUP.md` | OAuth walkthrough |
| `README.md` | This file |

`CHANGELOG.md` is written as reasoning, not as a list of edits: it records why
each change was made and what the wrong hypothesis was. It is the best guide to
the parts of this codebase that look arbitrary.

---

## Known limitations

1. **NeoPixel occasionally failed to start**, cleared by a reset. Traced in
   v10.6 to a mutex the library strands when its RMT init fails, which leaves
   every later `show()` a silent no-op. The channel is now claimed before the
   library asks for it and re-checked rather than latched. The Test page
   reports `buf` and `rmt` so a recurrence says which half is missing.
2. **Deep sleep can only wake on the shaft button** — a hardware limit of the
   S2's RTC GPIO range, not a software choice. Light sleep has no such limit
   and is the default.
3. **The battery curve is linear** between two configurable endpoints. Li-ion
   isn't: it falls fast from 4.2V to ~3.9V then plateaus, so the top of the
   range appears to drain quickly. Raising *0% mV* toward 3500 helps on a tired
   cell. A piecewise curve is the proper fix.
4. **WS2812s draw ~0.6–1 mA each whenever V+ is present**, even showing black —
   about 7–12 mA for the ring, far more than the board's sleep current. A
   P-MOSFET on the ring's V+ is the fix for true long-term standby (hardware on
   hand, not yet wired).
5. Credentials are stored in NVS in plain text and are readable by anyone with
   physical access to the board.

---

## Notes

Written for a specific desk. The mixer, soundbar and ring sizes are all
configurable, but the code assumes this hardware — it is published as a
reference and a record of the reasoning, not as a general-purpose library.
