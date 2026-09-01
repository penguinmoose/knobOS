# Changelog

**Current version: 10.1**

Major version = a new mini-app or a new subsystem. Minor version = tweaks, bug fixes, UI work.

Written as reasoning rather than as a list of edits: what each change was for, and where a wrong hypothesis was corrected. Reconstructed from the sketch's development history.

---

## 1.x — mini-app framework

### v1.0

Nav stack, App structs with null-checked handlers, config-page engine shared by menus and settings, Home screen with icons, Mixer Control (X32/M32 OSC over UDP), Servo Test. A+B back gesture with 45ms deferred dispatch. Servo detach-when-idle with settle re-baselining.

### v1.1

Compile fix: hoisted Glyph / BtnId / BtnEv / App / MenuItem above the first function, since the .ino preprocessor injects prototypes there referencing types not yet declared. Pot calibration set to the measured 1545 / 80 mV endpoints.

### v1.2

Chevrons reduced to 7x5, selection ring thickened. Replaced the useless FREE toggle (identical to the resting state) with HOLD, which energises the servo so back-drive force can be compared.

---

## 2.x — settings system

### v2.0

NVS persistence with debounced writes, System Settings, Servo configuration, Wi-Fi submenu (scan / manual / info with saved networks), Mixer settings, text entry, IP entry with validation, enum dropdowns, scrollbars, prerequisite launch gates.

### v2.1

Text entry reworked: knob mapped absolutely across the character set rather than by relative detents (nine sweeps to cross ~93 chars), with arming on cursor move so existing characters survive. Added click-into-character editing and an OK button. Same for IP entry.

---

## 3.x — Speaker Control mini-app

### v3.0

Sony Audio Control API over the LAN, SSDP discovery of ScalarWebAPI devices, speaker settings page, third home icon.

### v3.1

Settings entry relabelled Spotify -> Speaker.

### v3.2

Volume latency: persistent TCP, coalesced writes instead of one request per intermediate value, polling suppressed while turning (a stale poll was read as a remote change and drove the servo backwards). Added the Vol Min / Vol Max window.

### v3.3

Centre button switched to local mute, after the probe showed the soundbar never registers the Spotify Connect stream with avContent. Speaker Info became a scrollable text buffer.

---

## 4.x — Spotify Web API

### v4.0

Built-in OAuth config page served over the LAN, refresh-token storage, metadata polling, transport control, now-playing layout. Volume deliberately left on the local Sony API to stay instant.

### v4.1

Token exchange fixed: HTTP Basic credentials as documented, URL-encoded code, tolerant paste handling, full error surfaced.

### v4.2

Reclaimed the right margin (5px -> 2px), widened content. Merged with a branch adding uiWide() for label-less screens.

### v4.3

Column geometry corrected — divider and glyphs had moved by different amounts so the line read as drifting left. Icon boxes 28 -> 27px: an even box has no integer centre and every symbol leaned one pixel left.

### v4.4

Spotify mark baked in as a 27x27 bitmap; drawn at runtime it was either a WiFi fan (sweep too steep) or stepped at the apex.

### v4.5

Volume number made to track the knob directly. Remote changes now need the same value on two consecutive polls, removing the jump-up-then-back glitch. Volume UI moved to the bottom half.

### v4.6

market=from_token to shrink the player response, exponential back-off on failed polls, no network calls while the knob moves.

### v4.7

Network work moved to a dedicated FreeRTOS task — blocking HTTP in loop() had been freezing knob, display and buttons together. Mutex for the shared sockets, queued button presses, and the parser stopped calling substring() on a multi-kilobyte response.

### v4.8

Fixed a real deadlock: syRequest held the mutex while its 401 retry re-entered via the token refresh, and a plain FreeRTOS mutex is not recursive. Added a sync gate so entering the app no longer wrote the knob's resting position as the volume. Heap guard before TLS; netTask stack 12K -> 16K.

### v4.9

Spotify pretty-prints its JSON ("key" : value), so every parser matching "key": silently found nothing. Added whitespace-tolerant scanners. Response streams into a static buffer instead of a heap String, which had driven free heap down to ~13KB.

### v4.10

Version stamp and changelog.

---

## 5.x — scrubbing and About

### v5.0

Press-and-hold cue/review scrubbing with configurable hold time, speed, granularity and acceleration, all under a new Playback page. Elapsed and remaining/total clocks under the progress bar, both interpolated locally. Play/pause now freezes and resumes the local progress clock immediately rather than waiting for Spotify to confirm.

About screen with a KnobOS logo. Version scheme changed to major.minor and earlier entries renumbered.

### v5.1

Clawd mascot replaced with the real artwork, transposed from column-major to the row-major order drawBitmap wants, and the About footer re-laid out for a horizontal rather than vertical sprite.

### v5.2

Volume mapping anchored rather than absolute. The servo cannot park to better than a step or two, so quantising raw knob position invented a user change the moment a sync or a remote adoption finished — the jump on entry and the snap-back after adjusting at the soundbar were one bug, not two. Anchoring makes the delta zero at every re-anchor point, with re-anchoring at the stops to avoid wind-up, plus an 800ms write cooldown after adopting a remote change.

The back chord no longer fires its own members: suppressed buttons are excluded from release handling, and only chord members wait out the new Back Win delay while other buttons fire immediately. Both progress clocks derive from one elapsed-second value so they tick together. Marquee driven by the frame counter instead of millis(), which had been beating against the 50ms frame period.

"..." shown while a Spotify command is in flight. Divider moved to x=112. About logo enlarged to size 3 with letter spacing, dial centred on the letterform rather than the character cell. Fixed mixed-type max()/min() calls that fail to compile where uint32_t is unsigned long and size_t is unsigned.

### v5.3

Fixed the Spotify "..." indicator latching on permanently. It had been added by a mechanical text replacement that landed inside unbraced single-statement if bodies, so the flag escaped its condition and was set unconditionally every tick. Now set through a helper with explicit braces, only when Spotify is the transport backend, and with a 6s expiry so a missed clear cannot latch it again.

### v5.4

Reverted v5.2's anchored mapping, which fixed the phantom write by making volume relative to wherever the knob happened to be — so the scale drifted and the ends stopped reaching Vol Min / Vol Max. Volume is absolute again; the parking error that made absolute mapping misfire is now attacked at the source. The servo closes the loop on its own pot: after settling it measures the error and nudges again, up to four corrections, and only reports settled once it is within tolerance.

Post-move state is taken from the measured position rather than the commanded one. Remote changes are adopted only when they differ by two or more steps and at most every 1.2s, which stops the knob hunting while the soundbar's own volume is being adjusted.

### v5.5

Fixed the knob jumping nonstop on entry. On settle the app was adopting the knob's *measured* position as spaSent, so a park that fell short left spaSent disagreeing with spkVol; the remote-change detector then "corrected" it, parked short again, and looped forever. spaResync now records the speaker's value, and a motion gate stops the residual mismatch being written back — the mapping stays absolute, so full travel is still min..max.

Park tolerance and retry count exposed as Servo settings, since 1.5% of travel was tighter than the gearbox can repeat and the loop never converged. Verified with a real arduino-cli build for esp32:esp32:um_feathers2.

### v5.6

Mixer Demo mode. The IP entry screen gained Demo and Clear buttons reached by moving the cursor past the last digit, and Settings > Mixer gained Clear IP and Demo Mode rows. Demo drives the real Mixer UI — same strips, servo sync, meters and de-oscillation — from a local console model, intercepted at udpTx, mlTick and mlLinkOk so no UI code needed demo branches.

Nothing is sent on the wire and the flag is deliberately not persisted.

### v5.7

Speaker Control no longer requires a soundbar. The two halves are independent — volume is the Sony box on the LAN, metadata and transport are Spotify's cloud — so with no speaker configured the app runs as a pure Spotify remote. The launch gate now demands a speaker only when Spotify cannot carry the app alone, the sync gate no longer waits for a volume reading that will never arrive, the knob and volume writes are disabled rather than left half-live, and the lower half of the screen says so instead of showing a dead 0-50 bar.

Centre button falls back to play/pause since mute needs hardware.

### v5.8

Mixer console-to-knob sync no longer runs away. It still had the bug the speaker app shed in v5.5: after a park it took the knob's measured position as mxLastSent, which always disagreed with the console because the park lands short, so the next /xremote push looked like a fresh remote change and it parked again, forever. mxAdopt now records the console value, a motion gate stops the residual mismatch being read as input, remote changes need two matching reports, and a 1.2s cooldown follows each adopted move.

Added Settings > Mixer > Follow to disable console-to-knob driving entirely while leaving the display live.

### v5.9

Servo moves no longer abort mid-swing. svGoFader sized its hold time from the last *commanded* angle, which after any manual turning (servo limp) says nothing about where the shaft physically is — so when the stale angle sat near the target the hold came out near zero and the servo detached mid-swing, coasting to an arbitrary spot.

Travel is now estimated from the measured knob position, and the closed-loop trims scale their hold with the size of the correction instead of a fixed 110ms. This is why console-to-knob looked erratic while the speaker app seemed fine: mixer parks always follow heavy manual turning, speaker parks mostly follow servo moves where the commanded angle was fresh.

---

## 6.x — direction arbiter

### v6.0

Mixer sync rebuilt around an explicit owner (MX_IDLE / MX_KNOB / MX_MIXER) instead of letting both directions run at once behind echo windows, deadbands and confirmation counts. Those guards always overlapped somewhere: after a park the shaft relaxes a degree or two, the knob path read that as user input and sent it back, and the console moved the *wrong way*.

Now only one side owns the fader at a time — while the console drives, knob input is not merely filtered but ignored, and ownership is held through a 300ms relaxation window after the servo settles. Console reports arriving mid-move retarget the servo instead of queueing a stale destination, and claiming MX_MIXER requires the console value to have actually changed, which is what stops a short park being re-triggered by /xremote repeats.

Removed mxAdopt, mxBaseline, mxPendingMove and the two-report counter, all now redundant. The pot readout is replaced by "Knob -> Mixer" / "Knob <- Mixer", showing which side currently owns control.

### v6.1

Servo positioning rebuilt as a closed loop that keeps the servo ATTACHED throughout. The old scheme guessed a travel time, detached, let the knob coast, waited a fixed settle window, measured, and allowed one trim — every step of which could go wrong, and each detach let an analog servo relax before the next measurement.

The pot already reports position, so the guessing was never needed: command the feed-forward angle, hold it energised, wait until the shaft is genuinely STATIONARY, measure the residual and nudge, repeat until inside tolerance, and only then detach. New SV_SEEK state; SV_HOLD is now only used by the servo test page. Corrections are damped (Park Gain, default 60%).

At full gain a system whose real angle-to-fader gain is about twice the calibrated one — which 2:1 external gearing produces — overshoots every correction and rings at constant amplitude forever, never reaching target. That is the "moves erratically, never arrives" symptom. Under-correcting always converges; Park Trys raised to 4 to pay for the extra step or two.

---

## 7.x — rotary encoder

### v7.0

Servo and feedback pot replaced by a rotary encoder with a push switch. This is not a swap of one input device for another: the entire class of bugs that dominated 5.x and 6.x came from having a physical absolute position that had to be reconciled with a remote value. A relative encoder has no position to reconcile — a remote change simply overwrites the value and the next detent continues from there — so the servo state machine, the ownership arbiter, park/adopt/anchor, motion gates, echo windows, two-poll confirmations and Follow all deleted outright.

The mixer's sync logic went from roughly 90 lines to about 30, and the speaker's from about 80 to 25. Input: interrupt-driven quadrature decode on both edges of both channels through a 16-entry state table. Illegal transitions map to zero, so contact bounce is rejected by the decoder rather than by a debounce delay and no detent is missed however fast the knob is spun.

Optional acceleration for continuous values (fader, volume, character pick); menus deliberately do not use it, since skipping rows feels broken. Text and IP entry now step relatively and wrap, replacing the absolute mapping with its arming and hysteresis workarounds. Shaft button is a fourth button (B_ENC), never a member of the back chord.

Its action is chosen per app, mutually exclusive: Speaker = Play/Pause (default) | Mute | Next | Off, Mixer = Hold+Turn strip (default) | Click group | Off, Text = Caps Lock (default) | Confirm | Off. Mute is bound to a two-second hold rather than a click so it cannot silence the room by accident. Settings: Servo menu and Servo Test replaced by Encoder menu and a simpler Encoder Test showing detent count, direction, live acceleration multiplier, clicks and hold timing.

Every servo and pot setting removed; new Per Detent, Reversed, Accel, Accel Win, Fader/detent and Hold. Mixer loses Deadband, Min Move, Echo Tol, Echo Win and Follow, which existed only to police the servo. Pins: encoder A/B reuse the freed servo and pot pins (10, 7), the switch is on 11; all three use internal pull-ups, so no external parts.

ESP32Servo dependency dropped.

### v7.1

Shaft click now also acts as select on every generic UI screen — menus, settings, dropdowns, Wi-Fi scan and info, speaker scan, Home and IP entry — via a btnSelect() helper. Apps that give the shaft button its own meaning (Speaker, Mixer, Text entry, Encoder Test) handle B_ENC themselves and are untouched. Also repaired two things lost in a branch merge: the IP screen's Demo and Clear buttons existed only as declarations, so they were unreachable and undrawn, and the text-entry caps indicator had been rendering on the IP screen instead of the text screen.

### v7.2

Text entry gained a Clear button beside OK, reached the same way — keep pressing right past the last character. Clears every field that uses the editor, SSID and password included. The shaft click follows the selection when it sits on either button, so the encoder alone can drive the whole screen.

### v7.3

Caps lock is now a real mode. It had only remapped the current character to the matching letter in the other half of TI_CHARS, so the next rotation stepped straight back into the other case. The rotation order is filtered instead: only letters of the active case are offered, so the lock holds for everything typed afterwards, and toggling converts the character under the cursor in place.

Wi-Fi scan no longer hangs on "Scanning...". scanComplete() returns -2 on failure and the old code accepted only n >= 0, so a failed scan never resolved — and a scan started while the radio is busy associating fails routinely, which is why it worked in one place and not another. Failures and stalls now retry three times, then report it with a retry action.

Hidden SSIDs are included too. spkActive and syActive marked volatile: netTask reads them while the UI task writes them, so caching either was a real hazard. "No track info" split from "Contacting Spotify...", since one message covering both a completed empty poll and no poll at all made a silent polling failure look like an idle queue.

### v7.4

Serial.setTxTimeoutMs(0) when built with USB CDC on boot. On the S2's native USB a write blocks until the host drains it, up to 100ms each, so with no monitor attached the printing task stalls — netTask included. Speaker Info now also reports the Spotify side: configured, authed, wifi, active, poll count, last HTTP code, body size, track found, fail streak, free heap and the last error.

The device has to be able to explain itself on its own screen, because serial is unavailable unless USB CDC On Boot is enabled in the board menu. Added Settings > Speaker > Spotify > Poll Now to force a metadata fetch, and spaEnter queues the first poll directly rather than relying on syService() to schedule it.

### v7.5

Metadata polls were being blocked by my own heap guard. A live TLS session holds roughly 33KB of record buffers open between requests because of setReuse, so free heap settles near 12KB after the first call — below the 45KB floor. Transport commands slipped through in the moments heap happened to be recovered, which is why play/pause worked while nothing ever polled.

The floor is now conditional: 45KB when a handshake is needed, 12KB when reusing a live session, since the response body is a static buffer and needs almost no heap. Added syTlsIdle(): the session is dropped after 20s idle in-app, or 3s after leaving, returning the memory. 404 on a transport call is Spotify's NO_ACTIVE_DEVICE, not a failure — reported as "no active device" like a 204.

Error bodies from 4xx replies are now captured and their message shown, instead of a bare status code.

### v7.6

Wi-Fi scan reported "radio busy" almost instantly. scanComplete() returns -2 both for a failed scan and for no scan running, which is what a rejected start looks like, and the retry fired the moment it saw -2 — so all three attempts were spent inside a few hundred milliseconds, before the radio could possibly free up. Retries are now spaced 1.2s apart, up to seven, with the attempt counter shown.

WiFi.mode() is only called when the mode actually needs changing, since it resets the radio. Wi-Fi info: a saved network's detail view gained a Connect button, so a remembered network can be rejoined without retyping its password. Encoder Accel range starts at 1 rather than 0. Both 0 and 1 rendered as "Off", so the first press appeared to do nothing.

### v7.7

Mixer review before live use, two real bugs found: mxOnFader applied every inbound value even while the user was turning. Those are our own sends coming back through /xremote a round trip later, so they overwrote a fader the user had since moved further — the reading snapped backwards for a frame, worse the faster the spin.

Inbound values are now recorded but not applied while the user is active. That guard alone would have broken strip changes, since a new strip's fader would have been rejected as an echo, so a group/num change now clears user-active first. Added Hold = Fine: holding the shaft button divides the step by Fine Div (default 5) and suppresses acceleration, since multiplying the step would defeat the purpose.

Enc Btn is now Hold+Turn Ch | Hold = Fine | Click Group | Off.

### v7.8

Mixer fader step generalised from one rate plus a fine divisor to two independent rates. Rate A is the default and Rate B applies while the shaft button is held, with nothing requiring B to be the smaller of the two — setting B above A turns the hold into a coarse/fast mode instead of a fine one. Acceleration is opt-in per rate (Accel On: Off | Rate A | Rate B | Both), since it helps on a coarse rate and ruins a fine one, and which is which is now the user's choice rather than an assumption.

Fader/det moved off the Encoder page, where it was a mixer-only setting in a global menu.

### v7.9

Track info failing to appear when the speaker is unreachable was a starvation bug, not a Spotify one. netTask picked work from a fixed-priority chain with the volume poll at the top; an unreachable speaker costs a full connect timeout per attempt, and spkService re-queued one every 500ms, so netTask spent nearly all its time failing to reach a box that was not there and the Spotify poll below it never ran.

Transport still worked because commands are queued on demand and sit above the polls. Two fixes: the polls are now selected round-robin from a rotating cursor, so none can be crowded out however slow its neighbours are; and speaker polling backs off up to 16x once the speaker stops answering, resetting on the first success.

Commands still take priority over all polls.

---

## 8.x — NeoPixel ring

### v8.0

12-pixel NeoPixel ring on GPIO 7, pixel 0 at 6 o'clock running clockwise, driven as a gauge. Brightness is applied per pixel in software rather than through setBrightness(), because the clip indicator has to be able to ignore the master level entirely — a clip you might miss is not a clip indicator. Smoothing lights the pixel straddling the level proportionally to how far the level reaches into it, so the ring reads as continuous instead of twelve discrete steps.

Same idea as anti-aliasing a line; switchable off. Meter mode colours each pixel by ITS OWN position on the scale, not by the current level, which is what makes it read like a console meter rather than a bar that changes colour. Two to four zones with adjustable thresholds; default green to 65%, amber to 88%, red above.

Clip LED: Off | No Dim | Full Bright. Level modes can mark the extremes — first pixel at 0%, last at 100% — in a separate colour, and only at the extremes, so the ring is not permanently complaining. The zero case is drawn explicitly since at zero the scale itself lights nothing. New colour editor: RGB and HSV side by side as two views of one value, each rewriting the other, with the active column marked.

A/C step the six fields, the knob adjusts, holding the shaft button switches coarse to fine, and B cycles the live ring preview between all pixels, just the affected ones, and off. Settings > NeoPixel holds the on/off switch with Global, Mixer and Speaker submenus. Origin and Reverse are there too, so the ring can be mounted any way round without rewiring.

### v8.1

Battery and sleep. The plain FeatherS2 has NO on-board battery divider — the Neo has one on IO2 and the S3 on GPIO2, but the original board's own FAQ says there is no built-in way to read VBAT. So VBAT sensing expects an external divider on GPIO 3, with the ratio and both endpoints configurable. 5V presence is read from the divider on GPIO 10, and drives the charging bolt on the battery glyph.

Sleep: on the ESP32-S2 only GPIO0..21 are RTC-capable, so buttons B (38) and C (33) physically cannot wake the chip from deep sleep. Light sleep has no such limit, so it is the default — all four buttons wake it and the screen resumes exactly where it was, at a few hundred microamps instead of tens. Deep sleep is offered for storage and wakes on the shaft button.

Either way the wake press is swallowed: input is ignored until every button is released, so the press that wakes the device never also activates what is under it. Empty-battery shutdown wakes on the CHARGER line rather than on a timer. Polling to re-check the level would spend energy from an already flat pack and could not change the outcome — nothing but a charger raises the charge — whereas an RTC pin costs nothing while asleep and fires exactly when something can be done.

On charger wake the board runs radio-off in a charge-wait screen until the Resume At threshold, then restarts cleanly rather than bringing half-initialised subsystems up from a low-power state. Warnings latch at 20%% and 10%% and rearm on charge. Home and the mixer show icon plus percentage; back on Home opens a sleep menu with a larger readout.

Also fixed: ALL_APPS was still sized 24 while the list had grown to 32 entries, which was overrunning the array.

### v8.2

Sleep menu is now Sleep / Off, with the bottom button as back and a back glyph on the label column. Off always uses deep sleep whatever the default mode is — it is the "put it away" option, so the lowest draw is what is wanted — and wakes on the shaft button. NeoPixel review before first hardware test, two real faults: show() was being called on every loop pass, several hundred times a second, which achieves nothing visible and keeps the RMT peripheral busy for ~360us each time.

It now pushes only when the buffer has actually changed, and never faster than the display refresh. Meter zone thresholds could be edited out of order, and with t3 below t2 the zones swapped over; they are ordered at use so a bad setting degrades to a sensible meter instead of a scrambled one. The mixer battery readout moved to the true top-right: at y=10 it overlapped the size-2 dB figure.

The link indicator moved to the footer, which had room.

### v8.3

Hold the NeoPixel data line low across sleep. Clearing the ring sends zeros, but once the CPU sleeps GPIO7 stops being driven, and a floating WS2812 data input can latch noise into the pixels. On a ring powered straight from BAT that would keep draining the pack through a sleep that is supposed to cost microamps. The pad is now driven low and latched with gpio_hold_en/gpio_deep_sleep_hold_en, released on wake and at boot.

### v8.4

Spotify broke when the NeoPixel and battery code landed, and the cause was my own heap guard. The 32KB-vs-45KB floor for a fresh TLS handshake was measured when there was room to spare; the new globals and the RMT driver pushed idle heap just below it, so every handshake was refused before it started. The speaker kept working because it is plain HTTP with no guard at all.

Worse, syPoll then overwrote the specific "low heap" reason with a generic "no connection", which hid exactly the information needed. Floor lowered to what a handshake actually needs, and a specific error is never replaced by the generic one. "Speaker no response" while the speaker plainly worked was the v7.9 poll backoff outrunning a fixed 4s link window; the window now scales with the poll interval.

Occasional lost volume detents: a failed write was dropped, and the next adopt pulled the speaker's unchanged value back into the target, silently undoing the turn. Failed writes are now requeued twice. Sleep menu redrawn inside the content area — the buttons ran under the label column and the hint line fell off the bottom edge.

Mixer footer moved up 2px and the USER/IDLE tag dropped; it only reported whether the knob had moved recently, which the fader reading already shows. Battery reading gained a rolling average (Avg Window, 20s) plus a separate display latch (Update Ivl, 5s). The average smooths ADC noise; the latch stops the number twitching between two adjacent percentages when the average drifts across a boundary.

### v8.5

Charging bolt moved to the left of the battery glyph. Inside a 5px-tall body it was not legible; the layout widens by the bolt only while charging, so nothing shifts otherwise. Mixer reclaimed the row the USER/IDLE tag was using: the signal bar is now 11px instead of 9 and the rows are spread out. Zone-boundary ticks are drawn above it, one per boundary above zone 1, using the same thresholds as the ring so the OLED meter and the NeoPixel agree about where amber and red begin.

Fader percentage and link state now share the bottom row.

### v8.6

Both Spotify AND the speaker failing at once pointed at heap, not at either service. Two causes: the guard measured TOTAL free heap when a TLS handshake needs one large CONTIGUOUS block — a fragmented heap reports plenty free while the biggest usable piece is far too small — so it now checks getMaxAllocHeap(). And the kept-alive TLS session was pinning roughly 30KB for 20s at a time, which on this board is enough to starve the plain-HTTP speaker requests too.

That hold is now 2.5s in-app and immediate on exit, which is what makes the speaker snap-back and the Spotify -1 stop being the same bug wearing two faces. Button auto-repeat can be switched off: Rpt Delay accepts 0. Sleep menu: percentage first, battery to its right, bolt on the far left, hint text flush left so it clears the label column.

### v8.7

Spotify's "connect failed (-1)" was at least partly a timeout, not a name-resolution fault. v7.5 cut the TLS connect window to 2.5s, but an ECDHE handshake plus certificate work takes well over a second on this chip and DNS can add another, so a slow-but-healthy connection was being abandoned and surfaced as the same -1 a real DNS failure gives.

Raised to 9s for the token exchange and 8s for API calls; it all runs in netTask, so waiting costs the UI nothing. The token path also had no heap check at all, so a memory shortage there could only ever appear as a connection error. It now reports the largest free block, and the connect error carries that figure too, so the next failure says which of the two it is.

Sleep menu layout applied (percentage first, battery right, bolt far left, hint flush left) — these were written in v8.6 but the edit did not take.

### v8.8

"connect -1 blk 34804" settled it: the request passed the guard, attempted the handshake, and the allocation inside mbedTLS failed. It wants a larger CONTIGUOUS block than 34.8KB — its record buffers are 16KB each by default — so no amount of timeout or retry work could have helped. The fix is to give the internal heap more room: PSRAM must be enabled in the board menu, and the response buffer now allocates from PSRAM when present, returning 4.6KB of internal SRAM (static RAM fell from 89,620 to 85,012 bytes).

Boot logs psram/free/maxblk, and Speaker info shows PSRAM size and largest block, since without those the cause is invisible.

### v8.9

Spotify displaying but not responding was the 2.5s TLS hold from v8.6. That was a workaround for a heap shortage PSRAM has now removed, and it forced a fresh handshake on EVERY poll — seconds each, on a serial netTask — so transport commands queued behind one and appeared to do nothing. The session is now held for 120s when PSRAM is present, and commands are checked before polls rather than sharing the round-robin with them.

Added Settings > NeoPixel > Test: writes the ring directly, with no reference to npEnable, the per-app modes, the rate limiter or the dirty check. That separates a driver or wiring fault from app logic not calling the ring, which cannot be told apart from the outside. It also reports the library's pixel buffer pointer — a null there means its allocation failed and nothing will ever light, which otherwise looks exactly like bad wiring.

---

## 9.x — power and polish

### v9.0

Deep sleep woke the instant it was entered. Two causes, both needed: the ordinary GPIO pull-up is switched off when the digital domain powers down, so the shaft pin floated and read LOW — which IS the ext0 wake condition — and the button that selected "Off" was still held when sleep began. The RTC pull-up is now enabled and the release is waited for.

The charge-sense line gets an RTC pull-down for the same reason in reverse. Light sleep still acted on the wake press because the guard was evaluated and then ignored; buttonsPoll() is now skipped while it is held, so no press and no repeats. Mixer meter gained release-only smoothing (Meter Smooth, 55%). Rises are taken instantly and only the fall is damped, the way a hardware meter behaves — smoothing both directions would hide transients and make peaks late, which is the opposite of what a meter is for.

Battery glyph widened by one pixel so the first bar no longer sits against the border and read as part of it; the bolt is a pixel tighter to pay for it. Sleep screen uses fixed columns so nothing shifts when the charger comes and goes, with the voltage centred. Home icons down 2px, label down 1px, battery down 1px with the bolt still flush to the top edge.

Battery readout on the mixer is now optional, since it eats into long channel names.

### v9.1

Speaker AND Spotify dead while the mixer kept working was the tell: the mixer talks UDP straight from the UI task, while both of the others go through netTask behind a SINGLE shared mutex. A Spotify handshake or a stalled connect holds that lock for seconds, so the speaker's polls were timing out on the lock itself — reporting "no response" about a speaker that was answering fine.

They use separate client objects and never needed serialising against each other, so the lock is now split in two, with a short 2.5s wait for the LAN side and a long one for TLS. Also: a kept-alive TLS session that the far end has since closed fails on send rather than on connect and is indistinguishable from having no network.

Such a failure now drops the session and retries once with a fresh handshake, and the idle hold dropped from 120s to 45s, below the ~60s most servers allow. Sleep screen matches the home readout (bolt, battery, percentage), centred as a block with every slot reserved. Bolt 1px left.

### v9.2

"blk 34xxx" WITH PSRAM enabled was the missing piece. Enabling it in the board menu makes the external RAM visible but does not put it in the general malloc pool — the Arduino build reserves it for explicit ps_malloc. So 8MB sat unused while the largest allocatable block stayed near 34KB, and mbedTLS still could not get the ~40KB it needs. heap_caps_malloc_extmem_enable(8192) lowers the threshold above which malloc is served externally, so TLS record buffers go to PSRAM while small allocations stay internal.

Boot probes a 40KB malloc and checks whether it really landed in external RAM; the answer appears on the NeoPixel Test and Speaker info pages, so it can be verified without a serial cable. The heap guards defer to that probe, since getMaxAllocHeap reports the internal pool only and would otherwise keep refusing requests that can now succeed.

### v9.3

With "ext y" confirming PSRAM is in the malloc pool, memory is no longer the constraint and the remaining -1 is a real connection failure — consistent with the -80 dBm link. WiFi modem power save is now disabled and TX power set to maximum at every connect. Power save parks the radio between beacons, which costs little on a strong link but on a weak one becomes dropped packets and long retransmits; a TLS handshake is a chain of small round trips, so it fails where a single tiny LAN request still succeeds.

That asymmetry is exactly the "speaker fine, Spotify not" pattern. TLS timeouts raised to 15s/12s and a failed connect now always retries once after dropping any half-open session, not only when a kept-alive session was suspected. The display no longer flashes "Spotify idle" during a request: syErr is cleared on entry, so the idle text appeared for the whole time a request was in flight and read as a wrong answer rather than a pending one.

Requests are tracked and show "Contacting Spotify". Speaker info now reports RSSI, since signal strength has become a suspect worth being able to see.

### v9.4

"Spotify idle - no device" was mostly an artefact of my own error handling. syErr was cleared at the start of every request and again whenever a token refresh succeeded, so the last real failure was erased before it could be read, and the screen fell through to the idle text in the gap — which, with the failure backoff stretching to 32s, was most of the time.

The error now survives until a poll actually succeeds, the backoff is capped at 2x, and a 200 with no track distinguishes "playing nothing" from an unparsed item, since from the outside those look the same. The artist row, unused when there is no track, now carries the last HTTP code, body length and RSSI so the evidence is on the main screen rather than buried.

### v9.5

The speaker breaking again was my own regression. netTask is a single task, so while it sits in a Spotify connect it cannot service a queued volume write — and v9.3 raised the timeouts to 15s AND retried every failure, which could tie it up for most of a minute. Timeouts back to 5s/6s, the retry restricted to a reused session that may have been closed remotely, and a circuit breaker added: after four consecutive failures Spotify stands down for a minute, so a broken cloud link can never take the local controls with it.

For the -1 itself: the speaker is reached by IP and Spotify by name, so resolution is the one step only Spotify depends on. The host is now resolved explicitly before the token request, and a failure there reports as DNS rather than folding into the same -1 that a refused or timed-out connection produces.

### v9.6

Spotify moved onto its own task, which resolves a tension I had been trading back and forth for six versions without noticing it was a false choice. A TLS handshake genuinely needs several seconds on this chip, but netTask is serial, so any timeout long enough for Spotify also stalled the speaker's volume writes behind it.

Shortening the timeout fixed the speaker and broke Spotify; lengthening it did the reverse — which is exactly the oscillation v9.3 and v9.5 show. They are different workloads, one LAN-fast and one cloud-slow, and needed separate tasks rather than a compromise timeout suiting neither. Each already had its own mutex. netTask now handles only the Sony side with a small stack; syTask owns Spotify polling, transport and the TLS session lifetime, with the 16KB stack a handshake needs.

Command queues split to match, so the two cannot interleave. With the tasks independent the Spotify timeouts return to 12s/10s, which is what a real handshake needs and what v8.9 had when metadata last worked.

### v9.7

Reverted the two things added after v8.9, the last build where Spotify metadata worked, that could plausibly break a handshake. extmem malloc (v9.2) never changed the symptom — it failed identically before and after — and redirecting every allocation above 8KB into PSRAM is risky: the Wi-Fi driver allocates buffers on demand that must be DMA-reachable, and DMA cannot reach external RAM.

ESP-IDF defaults that threshold to 16KB for that reason. Maximum TX power (v9.3) raises the current spike during transmit, and on battery a sag mid-handshake is indistinguishable from a refused connection. Power-save-off is kept; it is a real win on a marginal link and carries no such risk. The internal headroom extmem was meant to buy arrived for free in v9.6 anyway: moving Spotify to its own task let netTask's stack drop from 16KB to 6KB, returning about 10KB — close to what the NeoPixel driver took when this first started failing. syBody still uses ps_malloc, which is contained rather than a global redirect.

### v9.8

Waking from DEEP sleep still delivered the wake press, because deep sleep resumes through a fresh boot and sleepResume() — where the wake guard is armed — never runs. The boot path now checks the wakeup cause and arms it, so the still-held button is swallowed the same way light sleep already handled it. Added Stuck Cut (System Settings, default 10s): a button held longer than that is treated as pressure rather than intent and ignored entirely until released, suppressing both its press and any repeats.

A device pressed against something in a bag can no longer repeat its way through a settings page. Auto-repeat can also still be switched off outright with Rpt Delay = 0.

### v9.9

"connect -1 blk 34xxx" is solved, and every PSRAM measure since v8.8 was aimed at the wrong heap. The Arduino core ships CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y, which pins EVERY mbedTLS allocation to internal RAM. mbedTLS therefore never touches the general allocator, so ps_malloc for syBody (v8.8) and heap_caps_malloc_extmem_enable (v9.2) could not reach it — and that is exactly why v9.2 "failed identically before and after".

The 8MB was real, visible, and unreachable by the one caller that needed it. What it needs is large. CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN is 16384 with ASYMMETRIC_CONTENT_LEN off, so a handshake allocates BOTH record buffers at full size: ~16.5KB each, ~33KB together, before the certificate chain. ESP.getMaxAllocHeap() reports the largest free INTERNAL block — the very pool in question — and it reads 32-38KB here.

So "blk 34804" was never a comfortable margin above a 32KB guard; it was a hair under what was actually being asked for. The first buffer fits, the second only just, the chain parse does not. A figure sitting on the boundary is also why this was intermittent for so long, why it "worked at v8.9", and why every timeout, retry and task change appeared to move it: they each shifted idle heap by a kilobyte or two across that line.

The fix is mbedtls_platform_set_calloc_free at boot, sending mbedTLS allocations of 1KB and up to PSRAM with an internal fallback. That is what CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC does at build time; a stock Arduino install cannot edit sdkconfig, so it is done at runtime. It is safe where the v9.7 revert was not: mbedTLS record buffers are filled by lwIP copies, never by DMA, and the Wi-Fi driver's DMA buffers do not come through mbedTLS at all.

Diagnosis, so the next failure cannot be ambiguous either. HTTP -1 flattens a refused socket, a timeout and an out-of-memory handshake into one number, which is what made this unfalsifiable for six versions. NetworkClientSecure keeps mbedTLS's own error across the stop() that follows a failed connect, so it is now read back and shown: "tls -0x7F00" is SSL_ALLOC_FAILED, "no socket" means TLS was never reached.

Speaker info gained tlspool (which heap mbedTLS is using), psblk, and the decoded error string. Two bookkeeping faults found alongside. syRequestOnce set syTlsLive unconditionally, so after a failed connect the guard believed a session was up and let the next request through on a third of the memory a handshake needs; it now reflects the result.

And syTokenRequest stopped the TLS client without clearing syTlsLive, leaving the same lie behind after every token refresh. The reuse-retry now samples the flag before the call, since checking it afterwards would no longer ever see a reuse. The heap floors were rewritten to describe the pool they actually guard: they now apply only on a board with no PSRAM.

---

## 10.x — per-network config, checkboxes

### v10.0

The console address is now per network, which is what it always was in reality. One global address meant carrying a venue's mixer IP onto the home network, where it pointed at nothing but still read as configured — so the mixer app opened, drew a working fader, and sent OSC into the void. That is the "it pretends like it works" case: nothing was wrong with the app, it had simply been told there was a console at an address where there was none.

Addresses are keyed by SSID rather than by an index into the saved network list, which reorders itself on every connect. The live address is adopted whenever the SSID changes, polled once a second rather than hooked to the connect path, since a reconnect, a roam and the first association after boot arrive by three different routes and all three have to be caught.

Once a second and not per pass because WiFi.SSID() builds a String, and doing that a few thousand times a second is exactly the heap churn this board cannot afford. With no address for the current network the link stays off the wire entirely and the mixer gate behaves as it does when nothing has ever been configured — straight to the IP screen, Demo button included.

The footer says "no IP" rather than "??", since a missing setting is not an unreachable mixer. Settings > Mixer > Networks... lists every network the device knows about with the address for each, assembled from three sources — the network we are on, the saved networks, and any stored address belonging to neither — so an address outlives its network being forgotten and can still be cleared afterwards.

An address configured before this existed is migrated at boot, not on first connect: the first adopt would find no entry, zero the live address and flush it, destroying the only copy. Which network it belonged to was never recorded, so the most recently saved one is the guess. Check Settings > Mixer > Networks after upgrading.

Entering the mixer with no Wi-Fi now offers Demo Mode from the Wi-Fi page itself. With no network there is no console either, so the simulator is the only thing the mixer can do from there. The row exists only in that context; PageWifi is the one page whose length is decided at runtime. Wi-Fi info is called "Past connections" while offline and opens straight into the history, since the current-connection page in that state exists only to be scrolled past.

Sleep: light sleep woke the instant it was entered if the button that chose it was still down. Both wake sources are LEVEL triggered, so a held button is not a press waiting to happen, it is the wake condition already true. Deep sleep waited for the shaft button because that is its only wake pin; light sleep arms all four and waited for none.

Now both wait, through one helper. The wake guard could never have helped here — the device was awake and correctly ignoring the button, which is a different fault from never having slept. Sleep menu: the top button switches between Sleep and Off, since with two choices a toggle beats a cursor, and the bottom button shows a return arrow instead of "<" — the same glyph this UI uses for "move left", which read as a direction rather than an action.

The wake hint also stopped claiming any button would wake it when the configured mode was deep. NeoPixel mixer mode gained Auto, which picks fader or meter per frame rather than making it a mode to remember to switch. Two independent conditions under Auto mode: fader just after a turn, and fader while the channel is quiet.

Independent because wanting the fader while adjusting and wanting it on a silent channel are unrelated reasons and any combination is sensible — which is why they are checkboxes rather than a two-option menu that would misrepresent them as alternatives. Fader Hold sets how long a turn counts for, to 100ms. A strip with no meter at all (MAIN, DCAs) counts as quiet, since the alternative there is an unlit ring.

Three additions to the config engine to support the above. C_CHECK is a bool that toggles on select rather than opening an edit mode, because dropping into one to change a two-state value you can already see buys nothing. Rows gained an optional visible() hook, so a setting that only means something in one mode is absent rather than sitting there doing nothing — sel now counts VISIBLE rows and every access goes through cfgItemAt().

And an optional dynLabel(), for a row that opens different things at different times and should say which. Every CfgPage now carries a static_assert tying its row-count literal to its array. Forgetting to bump that literal silently hides the new row, which has bitten this project repeatedly; it is a build error now. Writing them found nothing outstanding, which is the answer worth having.

### v10.1

With no speaker to send it to, the volume bar was a control that did nothing, and the knob drove it anyway. Both now switch to the playhead: a detent seeks by Knob Rate A, or Knob Rate B while the shaft button is held, which is the same two-rate arrangement the mixer fader uses and with nothing requiring B to be the larger. Acceleration is opt-in per rate. Spotify's seek does not interrupt playback, so the track keeps running throughout.

The sweep is local and only the resting position is sent, 400ms after the knob stops. Sending one seek per detent would both flood the API and lag behind the knob, since a round trip is a few hundred milliseconds; the press-and-hold scrub already worked this way.

In seek mode the shaft button is a rate selector as well as play/pause, so the click is decided on release. Acting on the press would have toggled playback every time the user reached for the second rate; a hold that moved the knob was a rate and nothing else.

Freed of the volume block, the progress bar takes the bottom third: twice the height, clocks moved clear of it, and the rate in use named underneath while seeking. Settings > Speaker > Knob overrides the choice (Auto | Volume | Progress) rather than leaving "no speaker" to be inferred, since a speaker that is merely switched off should not silently remap the control.

The NeoPixel speaker mode gained Progress and a second-rate colour. Its old "Volume" was really volume-with-a-speaker-and-progress-without, so that entry is relabelled Auto in place and keeps its stored value: the label was the inaccurate half, not the behaviour.

Mixer auto backlighting gained Quiet Delay. Music has gaps in it — a beat between phrases, a fade, the space between tracks — and every one of those snapped the ring to the fader for a frame. A strip with no meter at all is exempt: that is not silence, it is unmeterable, and no amount of waiting will change it.

The changelog moved out of the sketch into this file, so knob_os.ino is code.
