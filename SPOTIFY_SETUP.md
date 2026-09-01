# Spotify Web API setup

One-time setup. Takes about five minutes. Requires **Spotify Premium** — the
playback control endpoints reject free accounts with HTTP 403.

You never type the long tokens on the device. The knob serves a small config
page over your LAN, and you paste into that from a normal browser.

---

## 1. Create a Spotify app

1. Go to <https://developer.spotify.com/dashboard> and log in.
2. **Create app**. Name and description can be anything ("Knob OS").
3. For **Redirect URI**, enter this *exactly*, then click **Add**:

   ```
   http://127.0.0.1:8888/callback
   ```

   This precise string matters — it is baked into the firmware and Spotify
   rejects the token exchange if the two do not match character for character.
   It must be `127.0.0.1`, not `localhost`.

4. Under **Which API/SDKs are you planning to use**, tick **Web API**.
5. Save, then open the app's **Settings** and note the **Client ID**. Click
   **View client secret** to reveal the **Client Secret**.

> **Why a loopback address?** Spotify now only accepts HTTPS redirect URIs or
> loopback ones. The knob can't serve HTTPS, so it can't be the redirect
> target itself. The browser gets redirected to a dead loopback page, and you
> copy the code out of the address bar. Nothing actually has to listen on
> port 8888.

---

## 2. Open the knob's config page

On the device: **Settings → Speaker → Spotify → Setup...**

The screen shows an IP address. On a computer or phone on the same Wi-Fi,
browse to it, e.g. `http://192.168.1.24`.

Leave the device on that screen for the whole of this process — the web
server only runs while the page is open.

---

## 3. Enter credentials

Paste the **Client ID** and **Client Secret** into the form, then **Save**.
The status line should change to *credentials saved, not authorised*.

---

## 4. Authorise

1. Click **Open Spotify authorisation**. Log in and click **Agree**.
2. Your browser lands on a page that fails to load — something like
   *"This site can't be reached"* for `127.0.0.1:8888`. **This is expected
   and means it worked.**
3. Look at the address bar. It reads:

   ```
   http://127.0.0.1:8888/callback?code=AQBx7f...long...string
   ```

4. Copy everything **after** `code=` (not including `code=` itself). If a
   `&state=` appears afterwards, stop before the `&`.

---

## 5. Exchange the code

Paste it into the **Paste the code** box and click **Exchange**.

Status should read **authorised**. The refresh token is now saved to NVS and
survives reboots — you never repeat this unless you log out or change apps.

The authorization code is single-use and expires within a minute or two. If
you get *"Failed: token HTTP 400"*, just click **Open Spotify authorisation**
again for a fresh one.

---

## 6. Check it

Play something on the soundbar via Spotify Connect, then open
**Speaker Control** from the home screen. Within a few seconds you should see
the track title filling the top half, the artist below it, and a progress bar
along the bottom.

---

## Settings worth knowing

Under **Settings → Speaker**:

| Setting | Does what |
|---|---|
| **Bottom** | `Volume on turn` swaps the lower half to volume while you turn the knob; `Always volume` pins it there and hides artist/progress. |
| **Vol Hold** | Seconds the volume overlay lingers after you stop turning. |
| **Scroll** | Marquee speed for long titles, in ms per pixel. Lower is faster. |
| **Centre Btn** | `Auto` is play/pause when Spotify is authorised, mute otherwise. Force either explicitly if you prefer. |
| **Vol Min / Vol Max** | The knob's travel maps onto this window rather than the soundbar's full 0–50. |

Under **Settings → Speaker → Spotify**:

| Setting | Does what |
|---|---|
| **Transport** | Which backend handles skip/pause. `Spotify Web` is the one that works for Connect streams; `Sony local` only works for USB/Bluetooth/DLNA sources; `Off` disables the buttons. |
| **Meta Poll** | How often track info is fetched. 4 s is a good balance — the progress bar is interpolated locally between polls, so it moves smoothly regardless. |
| **Force Token** | Discards the cached access token and mints a new one. Useful for diagnosis. |
| **Log out** | Clears the refresh token. Client ID and secret are kept. |

---

## Troubleshooting

**"nothing playing"** — Spotify reports no active device. Start playback from
the Spotify app first; the knob controls an existing session, it can't start
one from nothing.

**Skip/pause do nothing, HTTP 403** — the account is not Premium, or the
`user-modify-playback-state` scope was not granted. Log out and redo step 4.

**HTTP 401 repeating** — the refresh token was revoked (password change, or
the app was removed from your account). Redo steps 4 and 5.

**Title shows but volume is laggy** — unrelated systems. Volume is the local
Sony API; check **Speaker info** for link status.

**Nothing at all after a reboot** — confirm the status line in *Setup...*
still says *authorised*. Settings are written to NVS about 1.5 s after the
last change, so avoid yanking power immediately after editing.

---

## A note on transport latency

Volume goes straight to the soundbar on your LAN and is effectively instant.
Skip and pause go out to Spotify's servers, which then push the command down
to the soundbar — expect roughly 200–500 ms. That is inherent to the route
and not something tuning can fix. It is the only route available, because the
HT-NT5's local API never sees the Spotify Connect stream at all.

## A note on TLS

The device talks to Spotify over HTTPS but does **not** verify the server
certificate (`setInsecure()`). Traffic is encrypted; server identity is not
checked. Pinning a root CA would be stronger but breaks the device whenever
Spotify rotates certificates, which is a poor trade for a knob on your own
LAN. Your client secret and refresh token are stored in NVS in plain text and
are readable by anyone with physical access to the board.
