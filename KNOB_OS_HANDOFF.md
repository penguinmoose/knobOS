# Knob OS — Project Handoff

**Current version: 9.9** · single file, `knob_os.ino`, ~6,450 lines
**Board: Unexpected Maker FeatherS2 (ESP32-S2)** · FQBN `esp32:esp32:um_feathers2`

A motorised-knob controller that became a rotary-encoder controller: a
mini-app OS driving a Midas M32R mixing console, a Sony HT-NT5 soundbar, and
Spotify, with a 12-pixel NeoPixel ring as a gauge and battery/sleep support.

---

## 1. Build

**Arduino IDE settings that matter:**

| Setting | Value | Why |
|---|---|---|
| Board | UM FeatherS2 | — |
| **USB CDC On Boot** | **Enabled** | Native USB. Without this `Serial` goes to the physical TX/RX pins and you get *no* output. Baud is irrelevant over CDC. |
| **PSRAM** | **Enabled** | 8MB. **Spotify does not work without it.** Used for the response buffer via `ps_malloc` *and*, since v9.9, for mbedTLS's TLS record buffers — see §4. With PSRAM off, Speaker info shows `tlspool internal` and handshakes fail. |

Libraries: Adafruit GFX, Adafruit SH110X, Adafruit BusIO, Adafruit NeoPixel.
(ESP32Servo was dropped at v7.0.)

Current footprint: **56% flash, 26% static RAM.**

Verified with `arduino-cli compile --fqbn esp32:esp32:um_feathers2:PSRAM=enabled`.

---

## 2. Hardware

### Pin map

| Function | GPIO | Notes |
|---|---|---|
| Encoder CLK (A) | 18 | also DAC2 |
| Encoder DT (B) | 17 | also DAC1 |
| Encoder SW | 0 | **BOOT strapping pin** — holding the shaft during reset enters the ROM bootloader (harmless, occasionally useful) |
| Button A (bottom) | 1 | OLED FeatherWing |
| Button B (middle) | 38 | OLED FeatherWing |
| Button C (top) | 33 | OLED FeatherWing |
| I²C SDA / SCL | 8 / 9 | UM variant pinout — **not** the Adafruit ESP32-S2 Feather's |
| NeoPixel data | 7 | |
| VBAT sense | 3 | ADC1_CH2, **external divider required** |
| 5V present | 10 | 5.1k:10k divider, RTC-capable so it can wake from deep sleep |

### Display
Adafruit 128×64 SH1107 OLED FeatherWing at 0x3C, **mounted upside down**
(rotation 3). Every direction word in the code is from the *user's*
perspective, not the panel's.

### Encoder (KY-040 or bare EC11)
`CLK→18, DT→17, SW→0, GND→GND`. `+` goes to **3V3, never 5V** — the S2 is not
5V tolerant and the module's pull-ups tie CLK/DT to whatever is on `+`. `+`
may also be left disconnected: all three lines use internal pull-ups.

### NeoPixel ring
12 pixels, **pixel 0 at 6 o'clock, index advancing clockwise**. V+ to **BAT,
not 3V3** — the regulator isn't meant to source it. A 3.3V data line is fine
at battery voltage because the WS2812 logic-high threshold is 0.7 × VDD
(≈2.6V at 3.7V). At 5V that threshold is 3.5V, which is why level shifters are
normally needed and aren't here.

Standing caveat: WS2812s draw ~0.6–1 mA *each* whenever V+ is present, even
showing black — ~7–12 mA for the ring, far more than the board's sleep
current. A P-MOSFET on the ring's V+ is the fix for true long-term standby
(planned, not implemented).

### Battery
**The plain FeatherS2 has no on-board battery divider.** This was verified
against the official FAQ — the FeatherS2 *Neo* has one on IO2 and the
FeatherS3 on GPIO2, but the original board does not. An external divider is
required: two equal resistors (e.g. 2× 100k) from BAT to GPIO 3, ratio 2.00.
Ratio and both endpoints are configurable.

---

## 3. Architecture

### Task model — the single most important thing to understand

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
the reverse. Versions 9.3 and 9.5 oscillate between exactly those two states.
They are different workloads — one LAN-fast, one cloud-slow — and needed
separate tasks, not a compromise timeout suiting neither (v9.6).

Each has its own recursive mutex (`netMutex`, `syMutex`). They must be
recursive: `syRequest`'s 401-retry path re-enters via the token refresh, and a
plain FreeRTOS mutex self-deadlocks there (v4.8).

**The UI never blocks on the network.** Apps set flags; the tasks act on them:

```
netVolWrite   queued volume level        netCmd   1 next 2 prev 3 pp 4 mute  (Sony)
netPollVol    poll speaker volume        syCmd    1 next 2 prev 5 play 6 pause 7 seek
netPollState  poll transport state       netPollSy  poll Spotify metadata
```

Polls are selected **round-robin** from a rotating cursor, not by fixed
priority — a strict chain let a slow, frequently-scheduled poll crowd out the
one below it forever (v7.9). Queued commands are checked before polls.

### App model

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

A nav stack pushes/pops these. All handlers are null-checked, so an app
implements only what it needs. `ALL_APPS[40]` indexes them for NVS nav
persistence — **grow this array when adding apps**; it was sized 24 while the
list had reached 32, silently overrunning (caught in v8.1).

**Detent ownership:** `loop()` consumes `knobSteps()` only for apps that
declare `onKnob`. Apps that read the encoder in their own `tick` must leave
`onKnob` null or the two compete.

### Display

- 20fps (`DISPLAY_MS = 50`), full redraw each frame.
- `CONTENT_W` is **mutable**, reset to narrow every frame; screens that want
  the full panel call `uiWide()` from their `draw()`.

| | value |
|---|---|
| Content (narrow) | 0–109 |
| Content (wide) | 0–125 |
| Divider | x=112 |
| Button glyphs | 117–125, centred at 121 |
| Keep-clear margin | 2px |
| Menu row width / scrollbar | 106 / x=107 |

Right-hand button-label column convention: bare glyphs, no `A:` prefixes.

### Buttons

Four (`B_C`, `B_B`, `B_A`, `B_ENC`). Events: press, repeat, release.

- **Back gesture is A+B.** Only chord members defer (by `Back Win`, default
  150ms); other buttons fire immediately so navigation stays snappy (v5.2).
- `B_ENC` is never a chord member.
- **`btnSelect()`** — generic screens treat the shaft click as another select.
  Apps that give it their own meaning (Speaker, Mixer, Text, Encoder Test)
  handle `B_ENC` themselves.
- **Wake guard**: after waking, input is ignored until *every* button is
  released. Armed in `sleepResume()` for light sleep and in `setup()` from the
  wakeup cause for deep sleep (deep sleep resumes through a fresh boot, so
  `sleepResume()` never runs — v9.8).
- **Stuck Cut** (default 10s): a button held longer is treated as pressure,
  not intent, and ignored entirely until released — both press and repeats.
  For a device in a bag.
- **Rpt Delay = 0** disables auto-repeat outright.

### Encoder

Interrupt-driven quadrature decode on both edges of both channels through a
16-entry state table. Illegal transitions map to 0, so bounce is rejected by
the *decoder* rather than by a debounce delay — no detent is missed however
fast it spins.

The encoder is **relative**, and that is the point. The servo it replaced had
an absolute position that constantly had to be reconciled with a remote value;
a relative encoder has nothing to reconcile — a remote change overwrites the
value and the next detent continues from there. That deleted the entire bug
class that dominated 5.x and 6.x.

`knobSteps()` returns detents; `knobStepsFast()` applies optional acceleration
(continuous values only — menus deliberately don't accelerate, since skipping
rows feels broken).

### Settings

Declarative `CfgItem` rows (int, bool, enum, action, link) in `CfgPage`
tables. **The page's item count is a separate literal — always update it when
adding a row**, or the new row is invisible. This has bitten repeatedly.

Persistence is NVS with debounced writes (~1.5s). Keys are ≤15 chars and must
be unique; there are ~80.

---

## 4. Devices and protocols

### Midas M32R (mixer) — UDP OSC, port 10023
- `/xremote` resubscribe every ~4s.
- Fader is 0..1 with a piecewise taper (0.75 = 0 dB).
- `/meters/0` = 70 little-endian floats: `[0..31]` ch, `[32..39]` aux,
  `[40..47]` fx, `[48..63]` bus, `[64..69]` mtx. MAIN/DCA have no meters.
- DCAs use `/fader`, not `/mix/fader`.
- X-Air variants report meters as int16 dBFS×256 (model setting exists).
- **Demo Mode** drives the real UI from a local console model, intercepted at
  `udpTx`, `mlTick`, `mlLinkOk`, `mlSendFader`, `mlRequestStrip`. No UI code
  has demo branches. The flag is deliberately not persisted.

### Sony HT-NT5 (speaker) — HTTP JSON, `http://<ip>:10000/sony/<service>`
No auth. Verified by probe:
- `audio.getVolumeInformation` 1.1 → volume/min/max/step/mute (0–50 on this unit)
- `audio.setAudioVolume` 1.1 — **volume is a STRING, not an int**
- `audio.setAudioMute` 1.1
- `avContent.getPlayingContentInfo` 1.2
- SSDP discovery: `urn:schemas-sony-com:service:ScalarWebAPI:1` → LOCATION →
  description XML `friendlyName`

**Critical finding:** this soundbar **never models the Spotify Connect stream**.
With music playing it still reports `state: STOPPED`, no title, no artist, no
position. It knows only that the active source is `netService:audio?service=spotify`.
So `pausePlayingContent` / `setPlayNextContent` have nothing to act on. This is
an architectural gap in the soundbar's firmware, not a parameter error — and it
is the entire reason Spotify Web API integration exists. Local transport verbs
*do* work for USB/Bluetooth/DLNA sources.

`switchNotifications` is websocket-only here, so state is polled.

### Spotify Web API — HTTPS
- OAuth2, refresh token in NVS. Requires **Premium**.
- Redirect URI must be exactly `http://127.0.0.1:8888/callback`. Spotify only
  accepts HTTPS or loopback, so the device can't host the callback; the user
  pastes the `code=` value into the device's own LAN config page. Full
  walkthrough in `SPOTIFY_SETUP.md`.
- Token endpoint needs **HTTP Basic** credentials, not body params.
- Poll `/v1/me/player?additional_types=track&market=from_token` — the `market`
  parameter drops two ~180-entry `available_markets` arrays.
- **Spotify pretty-prints its JSON** (`"key" : value`, spaces around the
  colon). Parsers matching `"key":` silently find nothing. The `cj*` scanners
  are whitespace-tolerant and allocation-free, scanning a static buffer.
- Parse anchor: `duration_ms`. Within `item` the keys run album, artists, …,
  duration_ms, …, name — so anchoring there yields the *track* name, and the
  last `artists` before it is the track's list rather than the album's. This is
  a key-ordering assumption and is the fragile part.
- 404 on a transport call is `NO_ACTIVE_DEVICE`, not a failure.

### TLS memory — read this before touching anything Spotify

The Arduino ESP32 core's shipped `sdkconfig` (checked for `esp32s2-libs`) says:

```
CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y        # mbedTLS is pinned to internal RAM
CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384
# CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN is not set   # so BOTH buffers are full size
```

Consequences, and they explain the entire v8.8 → v9.8 saga:

1. **mbedTLS never calls the general allocator.** `ps_malloc` for our buffers
   and `heap_caps_malloc_extmem_enable()` for `malloc` are invisible to it.
   That is why v9.2 "failed identically before and after".
2. **A handshake wants ~33KB of internal RAM in two ~16.5KB pieces**, plus the
   certificate chain. `ESP.getMaxAllocHeap()` is
   `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` — the exact pool —
   and it reads 32–38KB on this board. So `blk 34804` was never headroom above
   a 32KB guard; it was *just under the requirement*. Hence the intermittency.
3. **The fix (v9.9) is `mbedtls_platform_set_calloc_free()` at boot**,
   sending mbedTLS allocations ≥1KB to PSRAM with an internal fallback. This
   is the runtime equivalent of `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, which a
   stock Arduino install cannot set. Safe: mbedTLS record buffers are filled
   by lwIP copies, never DMA, and the hardware AES driver bounces external-RAM
   buffers through internal DMA memory (`esp_aes_process_dma_ext_ram`).

**Diagnosing a future `-1`.** HTTPClient collapses a refused socket, a
timeout and an out-of-memory handshake into the same `-1`.
`NetworkClientSecure::lastError()` keeps mbedTLS's real code across the
`stop()` that follows a failed connect, and v9.9 reads it back:

| On screen | Means |
|---|---|
| `tls -0x7F00` | `SSL_ALLOC_FAILED` — still out of memory in whichever pool |
| `tls -0x7780` | handshake failure alert from the far end (cipher/protocol) |
| `tls -0x2700`/`-0x3200` | certificate verification (should not happen; `setInsecure`) |
| `no socket` | TCP never opened — TLS was never reached at all |
| `DNS fail` | name resolution, checked explicitly before the connect |

Speaker info also shows `tlspool` (which heap mbedTLS is drawing from) and
`psblk` (largest free PSRAM block).

---

## 5. Feature summary

**Mixer Control** — 8 groups / 78 strips, fader + meter with peak hold and
zone-boundary ticks, two independent rates (A, and B while the shaft is held —
B may be *larger*, making hold a coarse mode), per-rate acceleration opt-in,
strip/group navigation, Demo Mode, optional battery readout.

**Speaker Control** — Sony volume (instant, LAN) plus Spotify metadata and
transport (cloud). Title/artist/progress/clocks, press-and-hold cue-and-review
scrubbing (hold time, speed, granularity, acceleration all configurable), local
progress clock that freezes instantly on pause rather than waiting for the
cloud. **Runs without a speaker** as a pure Spotify remote, and without Spotify
as a pure volume knob.

**NeoPixel ring** — gauge with smoothing (the pixel straddling the level lights
proportionally, so 12 pixels read as continuous), console-style zoned meter
(colour by *pixel position*, not by level), clip LED that can bypass both
dimming and master brightness, end markers, RGB/HSV colour editor with live
preview, origin/reverse for any mounting.

**Battery + Sleep** — SoC with rolling average and separate display latch,
charging bolt, warnings at 20/10%, flat-battery shutdown that wakes on the
*charger* line, light sleep (all four buttons wake, screen resumes in place) or
deep sleep (shaft button only — see below), sleep menu with Sleep / Off.

**System** — Wi-Fi scan/manual/saved networks with reconnect, text entry with
true caps-lock mode, IP entry with Demo/Clear buttons, About screen.

---

## 6. Debugging history — the lessons worth keeping

These recur. Reading this section will save re-deriving them.

**Absolute position is a liability.** The servo's parking error was
indistinguishable from user input, producing an entry jump, a snap-back after
adjusting at the soundbar, and a nonstop oscillation — all one bug wearing
different faces. Fixes that "made the guard stricter" never worked because the
architecture had a gap by construction. The encoder deleted the problem.

**One owner at a time.** The mixer's bidirectional sync was only fixed by an
explicit `MX_IDLE / MX_KNOB / MX_MIXER` arbiter. Echo windows and confirmation
counts always leave a gap somewhere.

**Total free heap is the wrong number.** `getFreeHeap()` sums holes;
`getMaxAllocHeap()` is the largest one, and that's what `malloc` needs. A 45KB
guard measured with headroom later blocked *every* TLS handshake once the
NeoPixel driver arrived — while the speaker (plain HTTP, no guard) kept
working. **A guard added to prevent a failure became the failure.**

**A number that looks like headroom may be the requirement.** `blk 34804`
was read for four versions as "plenty, so memory is not the problem". It was
in fact ~1KB short of what mbedTLS was asking for. Before treating a measured
figure as comfortable, find out what the thing you are measuring actually
needs — here that meant reading the core's shipped `sdkconfig`, not guessing.

**Check which allocator the failing code uses.** Three separate PSRAM fixes
(v8.8, v9.2, v9.7) all missed because mbedTLS is pinned to internal RAM by
`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC` and never calls `malloc` at all. "We gave
it 8MB" was true and irrelevant.

**Don't redirect malloc globally.** `heap_caps_malloc_extmem_enable(8192)`
seemed like the PSRAM fix but changed nothing, and it's risky: the Wi-Fi driver
allocates DMA-reachable buffers on demand and DMA cannot reach PSRAM. ESP-IDF
defaults that threshold to 16KB for a reason. Reverted at v9.7. Use explicit
`ps_malloc` for specific buffers instead.

**When two independent things fail together, look at what they share.** Both
Spotify and the speaker dying pointed at heap, then at the shared mutex, then
at the shared task — never at either service.

**When one works and the other doesn't, look at what differs.** Mixer (UDP,
UI task) fine while speaker and Spotify (netTask) failed → the task, not the
network. Speaker (by IP) fine while Spotify (by hostname) failed → DNS was
worth ruling out explicitly.

**Error messages must not be self-confirming.** `-1` covers DNS failure, TCP
refusal, timeout *and* allocation failure. My message said "TLS or DNS" and I
then read it back as evidence for DNS. Clearing `syErr` at request entry also
erased the only evidence, so the screen showed an idle message for the whole
time a request was in flight — which read as a wrong answer rather than a
pending one.

**Toolchain notes.** `min`/`max` are templates here: mixing `int` with `size_t`,
or `unsigned int` with `uint32_t`, fails to compile. Blind text replacement has
repeatedly landed inside unbraced single-statement `if` bodies — that's how the
Spotify `...` indicator latched on permanently. Always brace.

---

## 7. Known-open items

1. ~~**Spotify TLS connect fails with `-1`**~~ — diagnosed and fixed in v9.9.
   The internal heap could not hold mbedTLS's two 16KB record buffers; see the
   TLS memory section in §4. **Needs confirming on hardware:** Speaker info
   should read `tlspool psram`, and a failure should now name an mbedTLS error
   rather than a bare `-1`.
2. **NeoPixel fails to start ~20% of boots**, cleared by reset. Settings →
   NeoPixel → Test drives the ring directly and reports the library's buffer
   pointer; `buf NULL` would mean an allocation failure rather than wiring.
3. **Deep sleep can only wake on the shaft button.** GPIO 33 and 38 are outside
   the S2's RTC range (0–21) and physically cannot wake it. Light sleep has no
   such limit and is the default.
4. **Battery curve is linear** between configurable endpoints. Li-ion isn't
   linear — it falls fast from 4.2V to ~3.9V then plateaus — so the top of the
   range drains "quickly". Raising *0% mV* to ~3500 helps on a tired cell. A
   piecewise curve would be the proper fix.
5. **P-MOSFET for NeoPixel V+** — hardware on hand, not yet wired or coded.
6. Sony `magicPacketWakeSupported = 1`, so Wake-on-LAN for the soundbar is
   possible if desired.

---

## 8. Version map

| Era | What |
|---|---|
| 1.x | Mini-app framework, nav stack, Mixer Control |
| 2.x | Settings system, NVS, text/IP entry |
| 3.x | Speaker Control, Sony API, SSDP discovery |
| 4.x | Spotify Web API, OAuth, netTask, JSON parsing |
| 5.x | Scrubbing, About screen, Demo Mode, Spotify-only mode |
| 6.x | Mixer direction arbiter, closed-loop servo (superseded) |
| 7.x | **Rotary encoder replaces servo** — large deletion of sync code |
| 8.x | NeoPixel ring, battery and sleep, PSRAM |
| 9.x | Power polish, task split, network debugging, mbedTLS heap fix |

Full per-version changelog is in the comment block at the bottom of
`knob_os.ino`.

---

## 9. Files

| File | Purpose |
|---|---|
| `knob_os.ino` | The firmware. Everything. |
| `SPOTIFY_SETUP.md` | OAuth walkthrough |
| `tools/gen_icon.py` | Regenerates the 27×27 Spotify bitmap |
| `sony_api_probe.ino` | Standalone Sony API prober (superseded, useful reference) |
| `knob_hw_test.ino`, `m32r_knob.ino` | Early standalone sketches |

---

*Every change in this project has been compile-verified against
`esp32:esp32:um_feathers2` before delivery. Worth continuing — it has caught
declaration-order bugs, type-deduction failures and array overruns that were
invisible in review.*
