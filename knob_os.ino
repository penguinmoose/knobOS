/* ============================================================================
 *  KNOB OS  --  Unexpected Maker FeatherS2 (ESP32-S2)
 *  + Adafruit 128x64 OLED FeatherWing (SH1107), mounted upside down
 *  + rotary encoder with push-switch (EC11-style)
 *
 *  Home -> Mixer Control
 *       -> Settings -> System Settings
 *                   -> Encoder  -> Encoder Test / Configuration
 *                   -> Wi-Fi    -> Scan / Manual input / Wi-Fi info
 *                   -> Mixer    -> IP, model, tuning
 *                   -> Speaker  -> Scan / Manual IP / Speaker info
 *
 *  BOARD: "UM FeatherS2"  (NOT the Adafruit ESP32-S2 Feather)
 *  LIBS:  Adafruit SH110X, Adafruit GFX
 * ==========================================================================*/

/* ===========================================================================
 *  KNOB OS  --  version 9.9                   (full changelog at end of file)
 *
 *  Major version = a new mini-app or a new subsystem.
 *  Minor version = tweaks, bug fixes, UI work.
 * ========================================================================= */
#define KNOB_OS_VERSION "9.9"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <stdarg.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_NeoPixel.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

/* ###########################################################################
 * #                      FORWARD TYPE DECLARATIONS                          #
 * #                                                                         #
 * #  These MUST stay above the first function definition. The Arduino .ino  #
 * #  preprocessor injects auto-generated prototypes just before the first   #
 * #  function, so any type used in a signature has to be declared up here.  #
 * ######################################################################### */

enum Glyph { G_NONE, G_UP, G_DOWN, G_LEFT, G_RIGHT, G_SELECT, G_SELECT_OPEN,
             G_PLAY, G_PAUSE, G_NEXT, G_PREV, G_SPEAKER, G_MUTED, G_SUN };

enum BtnId { B_C = 0, B_B = 1, B_A = 2, B_ENC = 3, B_COUNT = 4 };
enum BtnEv { EV_PRESS, EV_REPEAT };

struct App {
  const char *name;
  void (*drawIcon)(int x, int y, int s);
  void (*onEnter)();
  void (*onExit)();
  void (*tick)();
  void (*draw)();
  void (*onButton)(BtnId b, BtnEv e);
  void (*onKnob)(int steps);
  void (*launch)();     // optional prerequisite gate, used instead of a push
  bool (*onBack)();     // return true if the app consumed the back gesture
};

// Config row kinds. C_LINK/C_ACT are navigation; the rest are editable values.
enum CfgType { C_LINK, C_ACT, C_INT, C_BOOL, C_ENUM, C_INFO };

struct CfgItem {
  const char *label;
  uint8_t type;
  int32_t *val;
  int32_t lo, hi, step;
  const char *const *opts;
  uint8_t nopts;
  const App *target;
  void (*action)();
  void (*fmt)(char *b, size_t n);   // custom value text, overrides default
  const char *unit;
};

struct CfgPage {
  const char *title;
  const CfgItem *items;
  uint8_t n;
  uint8_t sel;            // mutable: survives navigating into a submenu
  bool editing;
  const char *(*dynTitle)();
};

struct NvKey { const char *k; int32_t *v; int32_t def; };

struct TextReq {
  const char *title;
  char *buf;
  uint8_t cap;
  void (*onDone)();
};

/* ---- item construction macros (aggregate init must be positional) ------- */
#define ITEM_LINK(l,a)            { l, C_LINK, nullptr,0,0,0, nullptr,0, &a, nullptr, nullptr, nullptr }
#define ITEM_LINKF(l,a,f)         { l, C_LINK, nullptr,0,0,0, nullptr,0, &a, nullptr, f, nullptr }
#define ITEM_ACT(l,fn)            { l, C_ACT,  nullptr,0,0,0, nullptr,0, nullptr, fn, nullptr, nullptr }
#define ITEM_INT(l,v,lo,hi,st,u)  { l, C_INT,  &(v), lo,hi,st, nullptr,0, nullptr,nullptr,nullptr, u }
#define ITEM_BOOL(l,v)            { l, C_BOOL, &(v), 0,1,1, nullptr,0, nullptr,nullptr,nullptr,nullptr }
#define ITEM_ENUM(l,v,o)          { l, C_ENUM, &(v), 0,(int32_t)(sizeof(o)/sizeof(o[0]))-1,1, o,(uint8_t)(sizeof(o)/sizeof(o[0])), nullptr,nullptr,nullptr,nullptr }
#define ITEM_INFO(l,f)            { l, C_INFO, nullptr,0,0,0, nullptr,0, nullptr,nullptr, f, nullptr }

/* ###########################################################################
 * #                        FIXED HARDWARE CONFIG                            #
 * #        (things that describe the board, not user preferences)           #
 * ######################################################################### */

/* IP-entry screen state. Declared this early because mixerLaunch, far below,
 * arms the Demo button before the editor's own globals would appear. */
// Defined with the button helpers; used by screens declared before them.
static bool btnSelect(BtnId b);

static bool gIpAllowDemo = false;   // Demo button only offered for the mixer
static int  ipField = 0;            // 0 digits, 1 Demo, 2 Clear
static void (*gIpDemo)() = nullptr;

/* Rotary encoder. Works with a bare EC11 or with a KY-040 style breakout.
 *
 *   KY-040 pin   ->  here
 *   -----------      ----
 *   CLK          ->  PIN_ENC_A
 *   DT           ->  PIN_ENC_B
 *   SW           ->  PIN_ENC_SW
 *   +            ->  3V3  ** NOT 5V **
 *   GND          ->  GND
 *
 * The ESP32-S2 is NOT 5V tolerant. A KY-040's onboard pull-ups tie CLK/DT to
 * whatever is on its + pin, so powering it from 5V puts 5V straight onto
 * three GPIOs. Use 3V3.
 *
 * The + pin may also be left unconnected: every line below is configured
 * INPUT_PULLUP, and the encoder's common contact goes to GND either way. The
 * module's own pull-ups then simply sit unused. Board pull-ups in parallel
 * with the internal ones are harmless -- the result is just a stiffer pull-up.
 *
 * Note some KY-040 clones leave the SW pull-up resistor unpopulated; the
 * internal pull-up covers that case, so SW works regardless.
 *
 * Pin choices worth knowing about:
 *   17/18 are the two DAC outputs. Nothing here uses the DAC, so they are
 *   free -- but if audio output is ever wanted, move CLK/DT elsewhere first.
 *   0 is the BOOT strapping pin. It must be HIGH at reset to boot normally,
 *   which it is: the encoder switch is open when nobody is pressing it and
 *   the internal pull-up holds it high. Holding the shaft button down while
 *   resetting will drop the board into the ROM bootloader instead of running
 *   the sketch -- harmless, and occasionally useful for flashing. */
static const int PIN_ENC_A  = 18;   // CLK  (also DAC2 -- see note below)
static const int PIN_ENC_B  = 17;   // DT   (also DAC1)
static const int PIN_ENC_SW = 0;    // SW   (also the BOOT button)

/* NeoPixel ring, 12 pixels. Pixel 0 sits at 6 o'clock and the index advances
 * clockwise, so a level reads as a gauge sweeping up and around.
 *
 * Wire V+ to BAT, not 3V3: the ring can pull far more than the regulator is
 * meant to source. A 3.3V data line is fine at battery voltage because the
 * WS2812 logic-high threshold is 0.7 x VDD -- about 2.6V at 3.7V, which 3.3V
 * clears easily. (At 5V that threshold is 3.5V, which is why a level shifter
 * is needed there and not here.) */
/* Battery sensing.
 *
 * The plain FeatherS2 has NO on-board battery divider -- unlike the FeatherS2
 * Neo (IO2) and the FeatherS3 (GPIO2), the original board's FAQ states there
 * is no built-in way to read VBAT. So this expects an external divider from
 * BAT to PIN_VBAT, same idea as the 5V sense. Two equal resistors (e.g. two
 * 100k) give a ratio of 2.00; set Batt Ratio to match whatever you fit.
 *
 * PIN_VCHG is the 5V-present sense: a 5.1k:10k divider from the 5V pin gives
 * 5.0 * 10/15.1 = 3.31V, a safe logic high, and 0V when USB is absent. It is
 * an RTC GPIO, so it can also wake the board from deep sleep when a charger
 * is connected. */
static const int PIN_VBAT  = 3;     // external divider from BAT
static const int PIN_VCHG  = 10;    // 5V present, via 5.1k:10k
static const int PIN_NEO   = 7;
static const int NEO_N     = 12;
static const int PIN_BTN_A = 1;    // bottom
static const int PIN_BTN_B = 38;   // middle
static const int PIN_BTN_C = 33;   // top

static const uint8_t OLED_ADDR = 0x3C;
static const int I2C_SDA = 8;
static const int I2C_SCL = 9;

static const int SCREEN_W  = 128;
static const int SCREEN_H  = 64;
static const int CONTENT_W_NARROW = 110;   // leaves room for the label column
static const int CONTENT_W_WIDE   = 126;   // no labels: full width, 2px margin
static int CONTENT_W = CONTENT_W_NARROW;   // reset every frame by loop()
static const int DIVIDER_X = 112;   // nudged right; UI spacing unchanged
static const int LABEL_CX  = 121;          // glyphs span 117..125, 2px margin
// Column geometry: content 0..109, gap 110, divider 111, gap 112..116,
// glyphs 117..125, keep-clear 126..127. The divider-to-glyph gap stays at
// 5px as before; the whole column simply moved right by 3.
static const int LABEL_CY_C = 8, LABEL_CY_B = 32, LABEL_CY_A = 56;

static const int ROW_W    = 106;           // menu row width (scrollbar to right)
static const int ROW_H    = 12;
static const int ROW_TOP  = 14;
static const int ROWS_VIS = 4;
static const int SCROLL_X = 107;

static const uint32_t COMBO_MS   = 18;   // non-chord buttons: debounce only
static const uint32_t DISPLAY_MS = 50;
// Quadrature transitions per mechanical detent on a typical EC11.
static const int ENC_DEFAULT_DIV = 4;

// Protocol timings that have no reason to be user-visible.
static const uint32_t XREMOTE_MS = 4000;
static const uint32_t METER_MS   = 4000;
static const uint32_t RESYNC_MS  = 15000;
static const uint32_t SEND_MIN_MS = 50;
static const float    METER_FLOOR_DB = -60.0f;
static const uint32_t PEAK_HOLD_MS   = 1200;

static const uint32_t WIFI_TIMEOUT_MS = 20000;
static const int      MAX_SAVED_NETS  = 10;

/* ###########################################################################
 * #                    USER SETTINGS  (persisted in NVS)                    #
 * ######################################################################### */

// ---- System --------------------------------------------------------------
static int32_t sysKnobEdit   = 1;   // 0 = edits when merely selected, 1 = only when opened
static int32_t sysStartup    = 0;   // 0 Home, 1 Last app, 2 Last screen
static int32_t sysBackAct    = 0;   // 0 A+B, 1 B+C, 2 A+C, 3 A+B+C
static int32_t sysKnobInvert = 0;
static int32_t sysRepeatDly  = 450;
static int32_t sysRepeatRate = 110;
static int32_t sysComboMs    = 150;  // how long a chord member waits
static int32_t sysStuckSec   = 10;   // ignore a button held this long, 0 = off
static int32_t sysBright     = 200;
static int32_t sysFlip       = 1;   // 1 = rotation 3 (wing mounted inverted)

// ---- Encoder -------------------------------------------------------------
static int32_t encCfgDiv      = ENC_DEFAULT_DIV;  // transitions per detent
static int32_t encCfgInvert   = 0;    // swap if clockwise counts down
static int32_t encCfgAccel    = 1;    // 1 = off, else max multiplier
static int32_t encCfgAccelMs  = 40;   // detents faster than this accelerate
static int32_t encCfgHoldMs   = 2000; // shaft-button hold threshold

// ---- NeoPixel ------------------------------------------------------------
static int32_t npEnable    = 1;
static int32_t npBright    = 60;    // master brightness, applied in software
static int32_t npSmooth    = 1;     // fractional last pixel
static int32_t npOrigin    = 0;     // which physical pixel is the scale start
static int32_t npReverse   = 0;     // flip sweep direction
static int32_t npStep      = 10;    // colour editor coarse step

static int32_t npMxMode    = 1;     // 0 off, 1 fader, 2 meter
static int32_t npMxFadCol  = 0x0080FF;
static int32_t npMxRbCol   = 0xC040FF;
static int32_t npMxZones   = 3;
static int32_t npMxZ1      = 0x00FF00;
static int32_t npMxZ2      = 0xFFA000;
static int32_t npMxZ3      = 0xFF0000;
static int32_t npMxZ4      = 0xFF00FF;
static int32_t npMxT2      = 65;    // % of scale where zone 2 starts
static int32_t npMxT3      = 88;
static int32_t npMxT4      = 97;
static int32_t npMxClip    = 1;     // 0 off, 1 no dim, 2 no dim + full bright
static int32_t npMxEnds    = 1;     // mark 0% / 100% on the fader display
static int32_t npMxWarnCol = 0xFF0000;

static int32_t npSpMode    = 1;     // 0 off, 1 volume
static int32_t npSpCol     = 0x00A0FF;
static int32_t npSpEnds    = 1;
static int32_t npSpWarnCol = 0xFF0000;

// ---- Battery and sleep ---------------------------------------------------
static int32_t batSleepMin  = 5;      // idle minutes before sleeping
static int32_t batSleepMode = 0;      // 0 light (all buttons), 1 deep
static int32_t batRatio     = 200;    // divider ratio x100
static int32_t batMv0       = 3300;   // 0%
static int32_t batMv100     = 4200;   // 100%
static int32_t batWarnHi    = 20;     // first warning
static int32_t batWarnLo    = 10;     // second warning
static int32_t batResume    = 5;      // charge to this before normal operation
static int32_t batShowMixer = 1;      // battery readout on the mixer screen
static int32_t batAvgSec    = 20;     // rolling average window
static int32_t batUpdSec    = 5;      // how often the shown value is refreshed
/* Shaft-button action, chosen per app. Mutually exclusive by construction:
 * one action per app, not a set of overlapping toggles. Mute is bound to a
 * hold rather than a click so it cannot be triggered by accident. */
static int32_t encCfgTextBtn  = 0;    // 0 Caps Lock, 1 Confirm, 2 Off

// ---- Mixer ---------------------------------------------------------------
static int32_t mxCfgModel     = 0;    // 0 M32R/X32, 1 X-Air
static int32_t mxCfgIp        = 0;    // packed IPv4, 0 = not set
static int32_t mxCfgPort      = 10023;
static int32_t mxCfgUserHold  = 600;  // ms
static int32_t mxCfgMeterRate = 2;    // 50ms * this
static int32_t mxCfgEncBtn    = 0;    // 0 Hold+Turn strip, 1 Hold=fine, 2 Click, 3 Off
/* Two independent rates, both in 1/1000 of full fader travel per detent.
 * B is simply the rate used while the shaft button is held -- nothing says
 * it has to be the smaller one, so setting B above A turns the hold into a
 * coarse/fast mode instead of a fine one. */
static int32_t mxCfgRateA     = 10;   // 1.0% per detent
static int32_t mxCfgRateB     = 2;    // 0.2% per detent
static int32_t mxCfgAccelOn   = 1;    // 0 off, 1 rate A, 2 rate B, 3 both
static int32_t mxCfgMeterSm   = 55;   // meter release smoothing, %

// ---- Speaker (Sony Audio Control API) ------------------------------------
static int32_t spCfgIp        = 0;     // packed IPv4, 0 = not set
static int32_t spCfgPort      = 10000;
static int32_t spCfgPollMs    = 500;   // volume poll interval
static int32_t spCfgStateMs   = 2500;  // transport-state poll interval
static int32_t spCfgSendMs    = 120;   // min gap between volume writes
static int32_t spCfgUserHold  = 600;
static int32_t spCfgVolMin    = 0;     // knob maps to [VolMin..VolMax], which
static int32_t spCfgVolMax    = 50;    // is a soft range, not a hard stop
// This soundbar's avContent never sees the Spotify Connect stream, so its
// transport verbs do nothing. Mute is local and works, hence the default.
static int32_t spCfgCenterBtn = 0;     // 0 = Auto, 1 = Mute, 2 = Play/Pause
static int32_t spCfgEncBtn    = 0;     // 0 Play/Pause, 1 Mute (hold), 2 Next, 3 Off
static int32_t spCfgBottom    = 0;     // 0 = volume on turn, 1 = always volume
static int32_t spCfgVolHold   = 4;     // seconds the volume overlay lingers
static int32_t spCfgSyPoll    = 4000;  // Spotify metadata poll interval
static int32_t spCfgTransport = 0;     // 0 = Spotify Web, 1 = Sony local, 2 = off
static int32_t spCfgScroll    = 50;    // marquee speed, ms per pixel step
// Cue/review: hold a transport button to scrub through the track.
static int32_t spCfgSeekHold  = 450;   // hold before scrubbing starts, 0 = off
static int32_t spCfgSeekRate  = 15;    // song-seconds per real second
static int32_t spCfgSeekStep  = 100;   // update granularity, ms
static int32_t spCfgSeekAccel = 60;    // %/s speed growth, 0 = constant
static int32_t spCfgTimeRight = 0;     // 0 = remaining, 1 = total duration

// ---- Persisted navigation ------------------------------------------------
static int32_t navSaved0 = -1, navSaved1 = -1, navSaved2 = -1, navSaved3 = -1;

static const NvKey NVKEYS[] = {
  { "sKnobEdit",  &sysKnobEdit,   1   }, { "sStartup",   &sysStartup,    0   },
  { "sBackAct",   &sysBackAct,    0   }, { "sKnobInv",   &sysKnobInvert, 0   },
  { "sRepDly",    &sysRepeatDly,  450 },
  { "sRepRate",   &sysRepeatRate, 110 }, { "sBright",    &sysBright,     200 },
  { "sCombo",     &sysComboMs,    150 }, { "sStuck", &sysStuckSec,  10 },
  { "sFlip",      &sysFlip,       1   },
  { "eDiv",       &encCfgDiv,     ENC_DEFAULT_DIV }, { "eInv", &encCfgInvert, 0 },
  { "eAcc",       &encCfgAccel,   1   }, { "eAccMs",     &encCfgAccelMs, 40  },
  { "eHold",      &encCfgHoldMs,  2000},
  { "npEn",  &npEnable, 1 },      { "npBri",  &npBright, 60 },
  { "npSm",  &npSmooth, 1 },      { "npOrg",  &npOrigin, 0 },
  { "npRev", &npReverse, 0 },     { "npStp",  &npStep,  10 },
  { "npMxM", &npMxMode, 1 },      { "npMxF",  &npMxFadCol, 0x0080FF },
  { "npMxB", &npMxRbCol, 0xC040FF }, { "npMxN", &npMxZones, 3 },
  { "npMxZ1",&npMxZ1, 0x00FF00 }, { "npMxZ2", &npMxZ2, 0xFFA000 },
  { "npMxZ3",&npMxZ3, 0xFF0000 }, { "npMxZ4", &npMxZ4, 0xFF00FF },
  { "npMxT2",&npMxT2, 65 },       { "npMxT3", &npMxT3, 88 },
  { "npMxT4",&npMxT4, 97 },       { "npMxC",  &npMxClip, 1 },
  { "npMxE", &npMxEnds, 1 },      { "npMxW",  &npMxWarnCol, 0xFF0000 },
  { "npSpM", &npSpMode, 1 },      { "npSpC",  &npSpCol, 0x00A0FF },
  { "npSpE", &npSpEnds, 1 },      { "npSpW",  &npSpWarnCol, 0xFF0000 },
  { "btSlp", &batSleepMin, 5 },   { "btMode", &batSleepMode, 0 },
  { "btRat", &batRatio, 200 },    { "btMv0",  &batMv0, 3300 },
  { "btMv1", &batMv100, 4200 },   { "btWHi",  &batWarnHi, 20 },
  { "btWLo", &batWarnLo, 10 },    { "btRes",  &batResume, 5 },
  { "btAvg", &batAvgSec, 20 },    { "btUpd",  &batUpdSec, 5 },
  { "btMxS", &batShowMixer, 1 },
  { "mModel",     &mxCfgModel,    0   }, { "mIp",        &mxCfgIp,       0   },
  { "mPort",      &mxCfgPort,     10023}, { "mUserHold",  &mxCfgUserHold, 600 },
  { "mMeterRate", &mxCfgMeterRate,2   }, { "mEncBtn", &mxCfgEncBtn, 0 },
  { "mRateA",     &mxCfgRateA,    10  }, { "mRateB",  &mxCfgRateB,   2 },
  { "mAccelOn",   &mxCfgAccelOn,  1   }, { "mMtrSm", &mxCfgMeterSm, 55 },
  { "pIp",  &spCfgIp,   0    }, { "pPort", &spCfgPort,  10000 },
  { "pPoll",&spCfgPollMs,500  }, { "pState",&spCfgStateMs,2500 },
  { "pSend",&spCfgSendMs,120  }, { "pHold", &spCfgUserHold,600 },
  { "pVMin",&spCfgVolMin,  0  }, { "pVMax", &spCfgVolMax,  50  },
  { "pCtr", &spCfgCenterBtn, 0 }, { "pBot",  &spCfgBottom,    0 },
  { "pEnc", &spCfgEncBtn,    0 }, { "eText", &encCfgTextBtn,  0 },
  { "pVHold",&spCfgVolHold,  4 }, { "pSyP",  &spCfgSyPoll, 4000 },
  { "pTrans",&spCfgTransport,0 }, { "pScrl", &spCfgScroll,   55 },
  { "kHold", &spCfgSeekHold,450 }, { "kRate", &spCfgSeekRate, 15 },
  { "kStep", &spCfgSeekStep,100 }, { "kAccel",&spCfgSeekAccel,60 },
  { "kTimeR",&spCfgTimeRight, 0 },
  { "nav0", &navSaved0, -1 }, { "nav1", &navSaved1, -1 },
  { "nav2", &navSaved2, -1 }, { "nav3", &navSaved3, -1 },
};
static const int NVKEY_N = sizeof(NVKEYS) / sizeof(NVKEYS[0]);

static char wifiSsid[33] = "";
static char wifiPass[65] = "";
static char spName[28]   = "";     // soundbar friendlyName from discovery
static char syClientId[64] = "";
static char sySecret[64]   = "";
char syRefresh[220] = "";   // long-lived; access tokens live in RAM

Preferences prefs;
static bool     nvDirty = false;
static uint32_t nvDirtyMs = 0;

static inline bool elapsed(uint32_t since, uint32_t ms) {
  return (uint32_t)(millis() - since) >= ms;
}

/* Every HTTP call in this firmware blocks, and a blocking call in loop()
 * freezes the knob, the display and the buttons alike. The ESP32-S2 is single
 * core, but FreeRTOS still preempts, and a socket wait yields the CPU, so
 * moving the network work into its own task keeps the UI at full frame rate.
 * The UI only ever sets these flags; netTask does the talking. */
/* Two locks, not one.
 *
 * A single lock meant a Sony request had to wait out whatever Spotify was
 * doing. A TLS handshake or a stalled connect can hold it for seconds, so the
 * speaker's polls timed out on the lock itself and the link looked dead --
 * while the mixer, which talks UDP straight from the UI task and touches
 * neither, carried on working. They use separate client objects, so there was
 * never a reason to serialise them against each other. */
static SemaphoreHandle_t netMutex = nullptr;   // Sony / plain HTTP
static SemaphoreHandle_t syMutex  = nullptr;   // Spotify / TLS
static volatile int      netVolWrite = -1;    // queued volume level
static volatile int      netVolRetry = 0;     // attempts left for that write
/* Two queues, because they belong to two tasks now. netCmd is the LAN side
 * (Sony), syCmd the cloud side (Spotify). */
static volatile uint8_t  netCmd = 0;   // 1 sony next 2 sony prev 3 sony pp 4 mute
static volatile uint8_t  syCmd  = 0;   // 1 next 2 prev 5 play 6 pause 7 seek
static volatile int      netSeekMs = 0;
static volatile bool     syCmdPending = false;   // a Spotify command is in flight
static volatile uint32_t syCmdMs = 0;

// Declared beside the flags so every caller sees it directly.
static void syMarkPending() {
  if (spCfgTransport != 0) return;      // only Spotify commands show this
  syCmdPending = true;
  syCmdMs = millis();
}
static bool syPendingNow() {
  return syCmdPending && !elapsed(syCmdMs, 6000);
}

static volatile bool     netPollVol = false, netPollState = false, netPollSy = false;
static volatile bool     netBusy = false;
static int               netRR = 0;   // round-robin cursor over the polls

// Recursive: syRequest holds it while the 401 path re-enters via the token
// refresh. A plain mutex self-blocks there and wedges the whole task.
static bool netLock() {
  return !netMutex || xSemaphoreTakeRecursive(netMutex, pdMS_TO_TICKS(2500)) == pdTRUE;
}
static void netUnlock() { if (netMutex) xSemaphoreGiveRecursive(netMutex); }

static bool syLock() {
  return !syMutex || xSemaphoreTakeRecursive(syMutex, pdMS_TO_TICKS(15000)) == pdTRUE;
}
static void syUnlock() { if (syMutex) xSemaphoreGiveRecursive(syMutex); }

static void nvLoad() {
  prefs.begin("knobos", true);
  for (int i = 0; i < NVKEY_N; i++)
    *NVKEYS[i].v = prefs.getInt(NVKEYS[i].k, NVKEYS[i].def);
  prefs.getString("wSsid", wifiSsid, sizeof(wifiSsid));
  prefs.getString("wPass", wifiPass, sizeof(wifiPass));
  prefs.getString("spName", spName, sizeof(spName));
  prefs.getString("syId", syClientId, sizeof(syClientId));
  prefs.getString("sySec", sySecret, sizeof(sySecret));
  prefs.getString("syRef", syRefresh, sizeof(syRefresh));
  prefs.end();
}

static void nvFlush() {
  prefs.begin("knobos", false);
  for (int i = 0; i < NVKEY_N; i++) prefs.putInt(NVKEYS[i].k, *NVKEYS[i].v);
  prefs.putString("wSsid", wifiSsid);
  prefs.putString("wPass", wifiPass);
  prefs.putString("spName", spName);
  prefs.putString("syId", syClientId);
  prefs.putString("sySec", sySecret);
  prefs.putString("syRef", syRefresh);
  prefs.end();
  nvDirty = false;
}

// Debounced so a knob sweep across a value doesn't hammer the flash.
static void nvTouch() { nvDirty = true; nvDirtyMs = millis(); }
static void nvService() { if (nvDirty && elapsed(nvDirtyMs, 1500)) nvFlush(); }

static void nvFactoryReset() {
  prefs.begin("knobos", false);
  prefs.clear();
  prefs.end();
  ESP.restart();
}

/* ---- saved network list (unique by SSID, most recent first) -------------- */
static int nvNetCount() {
  prefs.begin("knobos", true);
  int n = prefs.getInt("nNet", 0);
  prefs.end();
  return constrain(n, 0, MAX_SAVED_NETS);
}

static void nvNetGet(int i, char *ssid, size_t sn, char *pass, size_t pn) {
  char ks[8], kp[8];
  snprintf(ks, sizeof(ks), "ns%d", i);
  snprintf(kp, sizeof(kp), "np%d", i);
  prefs.begin("knobos", true);
  prefs.getString(ks, ssid, sn);
  if (pass) prefs.getString(kp, pass, pn);
  prefs.end();
}

static void nvNetRemember(const char *ssid, const char *pass) {
  if (!ssid[0]) return;
  char s[MAX_SAVED_NETS][33], p[MAX_SAVED_NETS][65];
  int n = nvNetCount(), out = 0;

  strncpy(s[0], ssid, 32); s[0][32] = 0;
  strncpy(p[0], pass, 64); p[0][64] = 0;
  out = 1;
  for (int i = 0; i < n && out < MAX_SAVED_NETS; i++) {
    char es[33], ep[65];
    es[0] = ep[0] = 0;
    nvNetGet(i, es, sizeof(es), ep, sizeof(ep));
    if (!es[0] || !strcmp(es, ssid)) continue;      // dedupe
    strncpy(s[out], es, 32); s[out][32] = 0;
    strncpy(p[out], ep, 64); p[out][64] = 0;
    out++;
  }

  prefs.begin("knobos", false);
  prefs.putInt("nNet", out);
  for (int i = 0; i < out; i++) {
    char ks[8], kp[8];
    snprintf(ks, sizeof(ks), "ns%d", i);
    snprintf(kp, sizeof(kp), "np%d", i);
    prefs.putString(ks, s[i]);
    prefs.putString(kp, p[i]);
  }
  prefs.end();
}

/* ###########################################################################
 * #                    DISPLAY / UI PRIMITIVES                              #
 * ######################################################################### */

Adafruit_SH1107 display = Adafruit_SH1107(64, 128, &Wire);
WiFiUDP udp;

static uint32_t gFrame = 0;   // rendered-frame counter, drives the marquee

static int textW(const char *s) { return (int)strlen(s) * 6; }

// Screens that draw no button-label column can claim the whole panel.
static void uiWide() { CONTENT_W = CONTENT_W_WIDE; }
static int  uiCols() { return CONTENT_W / 6; }

static void printClipped(int x, int y, const char *s, int maxPx) {
  int maxChars = maxPx / 6;
  if (maxChars < 1) return;
  char buf[40];
  if (maxChars > (int)sizeof(buf) - 1) maxChars = sizeof(buf) - 1;
  snprintf(buf, sizeof(buf), "%.*s", maxChars, s);
  display.setCursor(x, y);
  display.print(buf);
}

static void drawGlyph(int cx, int cy, Glyph g, uint16_t col = SH110X_WHITE) {
  const int a = 3, b = 2;
  switch (g) {
    case G_UP:
      display.drawLine(cx - a, cy + b, cx, cy - b, col);
      display.drawLine(cx, cy - b, cx + a, cy + b, col);
      break;
    case G_DOWN:
      display.drawLine(cx - a, cy - b, cx, cy + b, col);
      display.drawLine(cx, cy + b, cx + a, cy - b, col);
      break;
    case G_LEFT:
      display.drawLine(cx + b, cy - a, cx - b, cy, col);
      display.drawLine(cx - b, cy, cx + b, cy + a, col);
      break;
    case G_RIGHT:
      display.drawLine(cx - b, cy - a, cx + b, cy, col);
      display.drawLine(cx + b, cy, cx - b, cy + a, col);
      break;
    case G_SELECT:      display.fillCircle(cx, cy, 3, col); break;
    case G_SELECT_OPEN: display.drawCircle(cx, cy, 3, col); break;
    case G_PLAY:
      display.fillTriangle(cx - 3, cy - 4, cx - 3, cy + 4, cx + 4, cy, col);
      break;
    case G_PAUSE:
      display.fillRect(cx - 3, cy - 4, 2, 9, col);
      display.fillRect(cx + 1, cy - 4, 2, 9, col);
      break;
    case G_NEXT:
      display.fillTriangle(cx - 4, cy - 4, cx - 4, cy + 4, cx + 1, cy, col);
      display.fillRect(cx + 2, cy - 4, 2, 9, col);
      break;
    case G_PREV:
      display.fillRect(cx - 4, cy - 4, 2, 9, col);
      display.fillTriangle(cx + 3, cy - 4, cx + 3, cy + 4, cx - 2, cy, col);
      break;
    case G_SUN: {
      display.fillCircle(cx, cy, 2, col);
      for (int a = 0; a < 8; a++) {
        float t = a * PI / 4.0f;
        int x1 = cx + (int)lroundf(cosf(t) * 4), y1 = cy + (int)lroundf(sinf(t) * 4);
        int x2 = cx + (int)lroundf(cosf(t) * 5), y2 = cy + (int)lroundf(sinf(t) * 5);
        display.drawLine(x1, y1, x2, y2, col);
      }
      break;
    }
    case G_SPEAKER:
    case G_MUTED:
      display.fillRect(cx - 4, cy - 2, 3, 5, col);
      display.fillTriangle(cx - 1, cy - 4, cx - 1, cy + 4, cx + 2, cy, col);
      if (g == G_MUTED) display.drawLine(cx - 4, cy + 4, cx + 4, cy - 4, col);
      else { display.drawFastVLine(cx + 4, cy - 2, 5, col); }
      break;
    default: break;
  }
}

static void drawButtonLabels(Glyph c, Glyph b, Glyph a) {
  display.drawFastVLine(DIVIDER_X, 0, SCREEN_H, SH110X_WHITE);
  drawGlyph(LABEL_CX, LABEL_CY_C, c);
  drawGlyph(LABEL_CX, LABEL_CY_B, b);
  drawGlyph(LABEL_CX, LABEL_CY_A, a);
}

static void drawBar(int x, int y, int w, int h, float frac) {
  display.drawRect(x, y, w, h, SH110X_WHITE);
  int f = (int)(constrain(frac, 0.0f, 1.0f) * (w - 2));
  if (f > 0) display.fillRect(x + 1, y + 1, f, h - 2, SH110X_WHITE);
}

static void drawTitle(const char *t) {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  printClipped(0, 0, t, CONTENT_W);
  display.drawFastHLine(0, 10, CONTENT_W, SH110X_WHITE);
}

static void drawCentered(int y, const char *s) {
  int x = (CONTENT_W - textW(s)) / 2;
  printClipped(x < 0 ? 0 : x, y, s, CONTENT_W);
}

static void drawScrollbar(int n, int first) {
  if (n <= ROWS_VIS) return;
  int h = ROWS_VIS * ROW_H;
  display.drawRect(SCROLL_X, ROW_TOP, 3, h, SH110X_WHITE);
  int th = max(4, h * ROWS_VIS / n);
  int ty = ROW_TOP + (h - th) * first / (n - ROWS_VIS);
  display.fillRect(SCROLL_X, ty, 3, th, SH110X_WHITE);
}

/* A menu row.
 *   selected + not editing -> whole row inverted
 *   selected + editing     -> row inverted, value re-inverted inside a chip,
 *                             so the field being changed reads as "open"     */
static void drawRow(int y, bool sel, bool editing, const char *label,
                    const char *value, Glyph tail) {
  if (sel) display.fillRect(0, y, ROW_W, ROW_H - 1, SH110X_WHITE);
  uint16_t fg = sel ? SH110X_BLACK : SH110X_WHITE;
  display.setTextColor(fg);

  int vw = value ? textW(value) : 0;
  int tailW = (tail != G_NONE) ? 9 : 0;
  int vx = ROW_W - 3 - vw - tailW;

  printClipped(3, y + 2, label, vx - 5);

  if (value && vw) {
    if (editing) {
      display.fillRect(vx - 2, y, vw + 4, ROW_H - 1, SH110X_BLACK);
      display.setTextColor(SH110X_WHITE);
    }
    display.setCursor(vx, y + 2);
    display.print(value);
    display.setTextColor(fg);
  }
  if (tail != G_NONE) drawGlyph(ROW_W - 6, y + ROW_H / 2 - 1, tail, fg);
  display.setTextColor(SH110X_WHITE);
}

/* ###########################################################################
 * #                       BATTERY AND SLEEP                                 #
 * #                                                                         #
 * #  Wake-source constraint worth knowing: on the ESP32-S2 only GPIO0..21   #
 * #  are RTC-capable, and deep sleep can only be woken by those. Buttons B  #
 * #  (38) and C (33) are outside that range, so they physically cannot wake #
 * #  the chip from deep sleep. Light sleep has no such limit -- any GPIO    #
 * #  can wake it -- at the cost of a few hundred microamps instead of tens. #
 * #  Light is therefore the default, since all four buttons work and the    #
 * #  screen resumes exactly where it was; Deep is offered for long storage. #
 * ######################################################################### */

RTC_DATA_ATTR static int  rtcWokeEmpty = 0;   // slept because the pack was flat
RTC_DATA_ATTR static int  rtcGoHome    = 0;   // slept from the sleep menu

static bool     gExtMalloc = false;   // did a large malloc land in PSRAM?
static volatile bool gSyBusy = false; // a Spotify request is in flight

/* ---- mbedTLS allocator: the actual cause of "connect -1 blk 34xxx" -------
 *
 * Every PSRAM measure since v8.8 was aimed at the wrong pool. The Arduino
 * core ships CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y, which pins EVERY mbedTLS
 * allocation to MALLOC_CAP_INTERNAL. So ps_malloc for our own buffers, and
 * heap_caps_malloc_extmem_enable() for the general allocator, could never
 * reach it: mbedTLS does not use the general allocator at all.
 *
 * And it is asking for a lot. CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN is 16384
 * with CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN switched OFF, so a handshake
 * allocates BOTH record buffers at full size -- roughly 16.5KB each, ~33KB
 * together, and the certificate chain on top of that. ESP.getMaxAllocHeap()
 * reports the largest free INTERNAL block, which is the very pool in
 * question, and on this board it reads 32-38KB. That is why the number in
 * the error message sat just above the guard and the handshake still failed:
 * the first buffer fits, the second only just, and the chain parse does not.
 * A figure that hovers on the boundary is also why this was intermittent for
 * so long and why every timeout, retry and task change moved it slightly.
 *
 * mbedTLS record buffers are never touched by DMA -- lwIP copies into them --
 * so external RAM is a perfectly valid home for them. That is exactly what
 * CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC does, and since a stock Arduino install
 * cannot change sdkconfig, the same thing is done at runtime here.
 *
 * Only blocks at or above TLS_PS_MIN are moved. Small, frequent crypto
 * allocations stay in fast internal RAM, so this is narrower than the global
 * extmem redirect reverted in v9.7 -- and unlike that one it cannot touch the
 * Wi-Fi driver's DMA buffers, because those never come through mbedTLS. */
static const size_t TLS_PS_MIN = 1024;
static bool gTlsPsram = false;        // mbedTLS allocations redirected to PSRAM

static void *tlsPsCalloc(size_t n, size_t sz) {
  if (sz && n > (SIZE_MAX / sz)) return nullptr;
  if (n * sz >= TLS_PS_MIN) {
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;                  // fall through to internal if PSRAM is full
  }
  return heap_caps_calloc(n, sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void tlsPsFree(void *p) { heap_caps_free(p); }

/* Must run before anything does crypto -- the Wi-Fi supplicant included --
 * so this is called at the very top of setup(). */
static void tlsHeapInit() {
  if (!psramFound()) return;
  gTlsPsram = (mbedtls_platform_set_calloc_free(tlsPsCalloc, tlsPsFree) == 0);
}

/* Modem power save is on by default. It parks the radio between DTIM beacons,
 * which costs little on a strong link but on a weak one turns into dropped
 * packets and long retransmits -- and a TLS handshake is a long chain of
 * small round trips, so it fails where a single tiny LAN request still gets
 * through. That asymmetry is exactly the "speaker works, Spotify does not"
 * pattern at -80 dBm. */
static void wifiTune() {
  /* Power save off is a genuine reliability win on a marginal link. Forcing
   * maximum TX power is not: it raises the current spike during transmit,
   * and on battery a sag mid-handshake looks exactly like a refused
   * connection. It was not set when this last worked, so it is left at the
   * driver's default. */
  WiFi.setSleep(false);
}
static float    batMvEma = -1.0f;
static uint32_t batLastRead = 0;
static uint32_t gLastActivity = 0;
static bool     gWakeGuard = false;   // ignore buttons until all are released
static int      batWarned = 0;        // 0 none, 1 high warning, 2 low warning
static bool     batChargeWait = false;

static bool batCharging() { return digitalRead(PIN_VCHG) == HIGH; }

/* Two separate time constants, because they solve different halves of the
 * jitter. The rolling average smooths ADC and load noise; the latch stops the
 * displayed number twitching even when the average moves slightly. Without
 * the latch a slow average still flickers between two adjacent percentages. */
static const uint32_t BAT_SAMPLE_MS = 250;
static float    batShownMv = -1.0f;
static uint32_t batLastUpd = 0;

static int batMv() {
  if (batMvEma < 0.0f || elapsed(batLastRead, BAT_SAMPLE_MS)) {
    batLastRead = millis();
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(PIN_VBAT);
    float mv = (acc / 8.0f) * ((float)batRatio / 100.0f);
    if (batMvEma < 0.0f) { batMvEma = mv; batShownMv = mv; }
    else {
      // Exponential average whose time constant is the configured window.
      float n = max(1.0f, (float)batAvgSec * 1000.0f / BAT_SAMPLE_MS);
      batMvEma += (mv - batMvEma) / n;
    }
  }
  if (batShownMv < 0.0f || elapsed(batLastUpd, (uint32_t)max((int)batUpdSec, 1) * 1000UL)) {
    batLastUpd = millis();
    batShownMv = batMvEma;
  }
  return (int)lroundf(batShownMv);
}

/* Linear between the configured endpoints. A LiPo's discharge curve is not
 * linear, but the flat middle means any simple mapping is approximate; the
 * endpoints are configurable so the reading can be trimmed to the pack. */
static int batSoc() {
  int span = max(1, (int)(batMv100 - batMv0));
  return constrain((batMv() - (int)batMv0) * 100 / span, 0, 100);
}

static void batNoteActivity() { gLastActivity = millis(); }

/* Battery glyph. Five fill states plus a bolt when 5V is present. */
/* Lightning bolt, drawn to the LEFT of the battery rather than inside it:
 * at scale 1 the body is only 5px tall, and a bolt inked into that is not
 * legible. */
static void drawBolt(int x, int y, int scale) {
  int u = scale;
  display.fillTriangle(x + 3*u, y, x, y + 5*u, x + 3*u, y + 5*u, SH110X_WHITE);
  display.fillTriangle(x + 2*u, y + 9*u, x + 5*u, y + 4*u, x + 2*u, y + 4*u, SH110X_WHITE);
}

static void drawBattery(int x, int y, int soc, int scale) {
  // 14 wide rather than 13: at 13 the first bar sat against the border and
  // read as part of it, so two bars looked like one.
  int w = 14 * scale, h = 7 * scale;
  display.drawRect(x, y, w, h, SH110X_WHITE);
  display.fillRect(x + w, y + 2 * scale, scale, h - 4 * scale, SH110X_WHITE);

  int bars = (soc >= 88) ? 4 : (soc >= 63) ? 3 : (soc >= 38) ? 2 : (soc >= 13) ? 1 : 0;
  for (int i = 0; i < bars; i++)
    display.fillRect(x + 2 * scale + i * 3 * scale, y + scale, 2 * scale,
                     h - 2 * scale, SH110X_WHITE);
  if (!bars && soc > 0)   // nearly empty: a sliver rather than nothing
    display.fillRect(x + 2 * scale, y + scale, scale, h - 2 * scale, SH110X_WHITE);
}

// Icon plus percentage, right-aligned so it sits neatly in a corner.
static void drawBatteryStatus(int rightX, int y, int scale) {
  int soc = batSoc();
  bool chg = batCharging();
  char t[8];
  snprintf(t, sizeof(t), "%d%%", soc);
  int tw = (int)strlen(t) * 6 * scale;
  int bw = 15 * scale;              // body plus the terminal nub
  int boltW = chg ? (6 * scale) : 0;   // 1px tighter to pay for the wider body
  int x = rightX - (boltW + bw + 3 + tw);
  if (chg) drawBolt(x - 1, y - scale, scale);   // 1px left, clears the body
  drawBattery(x + boltW, y, soc, scale);
  display.setTextSize(scale);
  display.setCursor(x + boltW + bw + 3, y);
  display.print(t);
  display.setTextSize(1);
}

/* ---- sleep ------------------------------------------------------------- */
static void sleepPrepare() {
  npClear();
  /* Clearing sends zeros, but once the CPU sleeps GPIO7 stops being driven
   * and a floating WS2812 data line can latch noise into the pixels -- which
   * on a ring powered straight from BAT means it keeps draining. Drive the
   * line low and latch that state so it survives sleep. The short delay lets
   * the clear frame plus its reset gap finish first. */
  delay(5);
  pinMode(PIN_NEO, OUTPUT);
  digitalWrite(PIN_NEO, LOW);
  gpio_hold_en((gpio_num_t)PIN_NEO);
  gpio_deep_sleep_hold_en();
  display.clearDisplay();
  display.display();
  display.oled_command(SH110X_DISPLAYOFF);
}

static void sleepResume() {
  gpio_hold_dis((gpio_num_t)PIN_NEO);
  gpio_deep_sleep_hold_dis();
  display.oled_command(SH110X_DISPLAYON);
  gWakeGuard = true;             // the wake press must not also act
  knobResetSteps();
  batNoteActivity();
}

static void enterSleep(bool fromMenu, bool forceDeep = false) {
  rtcGoHome = fromMenu ? 1 : 0;
  navSaveNow();
  nvFlush();
  sleepPrepare();

  if (batSleepMode == 0 && !forceDeep) {
    /* Light sleep: every button can wake it because GPIO wake is not limited
     * to RTC pins. Wi-Fi is stopped first, otherwise the radio keeps waking
     * the CPU for beacons and the saving is largely undone. */
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    const int pins[4] = { PIN_BTN_A, PIN_BTN_B, PIN_BTN_C, PIN_ENC_SW };
    for (int i = 0; i < 4; i++)
      gpio_wakeup_enable((gpio_num_t)pins[i], GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();

    WiFi.mode(WIFI_STA);
    if (wifiConfigured()) { WiFi.begin(wifiSsid, wifiPass); wifiTune(); }
    sleepResume();
    if (fromMenu) navHome();
    return;
  }

  /* Deep sleep: ext0 takes a single RTC pin, so the shaft button is the wake
   * key. B and C are not RTC-capable and cannot serve here.
   *
   * Two things have to be right or it wakes immediately. The normal GPIO
   * pull-up is switched off when the digital domain powers down, so the pin
   * floats and reads LOW -- which is exactly the wake condition. The RTC
   * pull-up has to be enabled instead. And the button that selected "Off" is
   * still down at this point, so wait for it to come up first. */
  rtc_gpio_pullup_en((gpio_num_t)PIN_ENC_SW);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_ENC_SW);
  uint32_t t0 = millis();
  while (digitalRead(PIN_ENC_SW) == LOW && millis() - t0 < 5000) delay(10);
  delay(60);                                   // let the contact settle
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_ENC_SW, 0);
  esp_deep_sleep_start();
}

/* Flat battery. Sleep with the charger line as the only wake source.
 *
 * Polling on a timer to re-check the level would spend energy from an already
 * empty pack, and could not change the outcome anyway: nothing but a charger
 * raises the charge. Waking on the charger line costs nothing while asleep
 * and fires exactly when something can actually be done. */
static void enterEmptySleep() {
  rtcWokeEmpty = 1;
  navSaveNow();
  nvFlush();
  sleepPrepare();
  // Same reasoning in reverse: hold the charge-sense line down so a floating
  // input cannot look like "charger connected".
  rtc_gpio_pulldown_en((gpio_num_t)PIN_VCHG);
  rtc_gpio_pullup_dis((gpio_num_t)PIN_VCHG);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_VCHG, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

/* ###########################################################################
 * #                        NEOPIXEL RING                                    #
 * #                                                                         #
 * #  A 12-pixel ring used as a gauge. Pixel 0 is at 6 o'clock and the index #
 * #  runs clockwise, so a level sweeps up and around like a dial.           #
 * #                                                                         #
 * #  Brightness is applied in software rather than through the library's    #
 * #  setBrightness(), because the clip indicator has to be able to ignore   #
 * #  the master level entirely -- a clip you might miss is not a clip       #
 * #  indicator.                                                             #
 * ######################################################################### */

static Adafruit_NeoPixel ring(NEO_N, PIN_NEO, NEO_GRB + NEO_KHZ800);
static uint8_t npBuf[NEO_N][3];
static bool    npTouched = false;   // did anything draw this frame?
static bool    npLit = false;       // is anything currently lit?

static inline uint8_t npR(int32_t c) { return (uint8_t)((c >> 16) & 0xFF); }
static inline uint8_t npG(int32_t c) { return (uint8_t)((c >> 8) & 0xFF); }
static inline uint8_t npB(int32_t c) { return (uint8_t)(c & 0xFF); }
static inline int32_t npRGB(int r, int g, int b) {
  return ((int32_t)(r & 0xFF) << 16) | ((int32_t)(g & 0xFF) << 8) | (b & 0xFF);
}

static void npBegin() {
  // Release any pad hold left over from a previous deep sleep.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_NEO);
  ring.begin();
  ring.setBrightness(255);          // scaling is done per pixel below
  ring.clear();
  ring.show();
  /* If the pixel buffer is null the library's malloc failed and nothing will
   * ever light, which is otherwise indistinguishable from a wiring fault. */
  Serial.printf("[np] pin=%d n=%d buf=%s\n", PIN_NEO, NEO_N,
                ring.getPixels() ? "ok" : "NULL");
}

static void npFrameStart() { memset(npBuf, 0, sizeof(npBuf)); npTouched = true; }

/* Map a logical scale position onto a physical pixel, honouring the origin
 * offset and direction so the ring can be mounted any way round. */
static int npMap(int i) {
  int n = NEO_N;
  int k = npReverse ? (n - 1 - i) : i;
  k += (int)npOrigin;
  k %= n;
  if (k < 0) k += n;
  return k;
}

// level 0..1 of this pixel's own contribution, colour scaled by it
static void npSet(int i, int32_t col, float level, bool ignoreMaster) {
  if (i < 0 || i >= NEO_N) return;
  level = constrain(level, 0.0f, 1.0f);
  float m = ignoreMaster ? 1.0f : (float)constrain(npBright, 1, 255) / 255.0f;
  float f = level * m;
  int p = npMap(i);
  npBuf[p][0] = (uint8_t)lroundf(npR(col) * f);
  npBuf[p][1] = (uint8_t)lroundf(npG(col) * f);
  npBuf[p][2] = (uint8_t)lroundf(npB(col) * f);
}

/* Push to the ring only when something actually changed, and never faster
 * than the display refresh. show() was previously called on every loop pass
 * -- several hundred times a second -- which achieves nothing visible and
 * keeps the RMT peripheral busy for ~360us each time. */
static uint8_t  npSent[NEO_N][3];
static uint32_t npLastShow = 0;

static void npFrameEnd() {
  if (!npEnable) return;
  bool changed = memcmp(npSent, npBuf, sizeof(npBuf)) != 0;
  if (!changed || !elapsed(npLastShow, DISPLAY_MS)) { npLit = true; return; }
  npLastShow = millis();
  memcpy(npSent, npBuf, sizeof(npBuf));
  for (int i = 0; i < NEO_N; i++)
    ring.setPixelColor(i, ring.Color(npBuf[i][0], npBuf[i][1], npBuf[i][2]));
  ring.show();
  npLit = true;
}

static void npClear() {
  if (!npLit) return;
  ring.clear();
  ring.show();
  memset(npSent, 0, sizeof(npSent));
  npLastShow = millis();
  npLit = false;
}

/* Colour for a pixel sitting at position `pos` (0..1) along a zoned scale.
 * Zones are chosen by where the PIXEL is, not by the current level, which is
 * what makes it read like a console meter rather than a colour-changing bar. */
static int32_t npZoneAt(float pos) {
  int zones = constrain((int)npMxZones, 2, 4);
  /* Nothing stops the thresholds being edited out of order, and if t3 fell
   * below t2 the zones would swap over. Order them here so a bad setting
   * degrades to a sensible meter instead of a scrambled one. */
  float t2 = npMxT2 / 100.0f;
  float t3 = max(t2, npMxT3 / 100.0f);
  float t4 = max(t3, npMxT4 / 100.0f);
  if (zones >= 4 && pos >= t4) return npMxZ4;
  if (zones >= 3 && pos >= t3) return npMxZ3;
  if (pos >= t2) return npMxZ2;
  return npMxZ1;
}

/* Draw a level as a scale.
 *
 * With smoothing on, the pixel straddling the level is lit proportionally to
 * how far into it the level reaches, so the ring reads as continuous rather
 * than as twelve discrete steps -- the same idea as anti-aliasing a line.  */
static void npScale(float frac, int32_t flatCol, bool zoned,
                    bool endsWarn, int32_t warnCol,
                    bool clipPix, bool clipFull) {
  frac = constrain(frac, 0.0f, 1.0f);
  float lit = frac * NEO_N;

  for (int i = 0; i < NEO_N; i++) {
    float level;
    if (lit >= (float)(i + 1))      level = 1.0f;
    else if (lit <= (float)i)       level = 0.0f;
    else                            level = npSmooth ? (lit - (float)i) : 1.0f;
    if (level <= 0.0f) continue;

    int32_t col = zoned ? npZoneAt(((float)i + 0.5f) / NEO_N) : flatCol;

    // Ends only turn warning-coloured at the actual extremes, so the ring
    // does not permanently look like it is complaining.
    if (endsWarn && i == 0 && frac <= 0.0005f)          col = warnCol;
    if (endsWarn && i == NEO_N - 1 && frac >= 0.9995f)  col = warnCol;

    bool isClip = clipPix && (i == NEO_N - 1);
    if (isClip) npSet(i, col, 1.0f, clipFull);   // never dimmed by level
    else        npSet(i, col, level, false);
  }

  /* At zero the loop above lights nothing, so the "at minimum" marker would
   * never appear. Light it explicitly -- that state is exactly the one worth
   * flagging. */
  if (endsWarn && frac <= 0.0005f) npSet(0, warnCol, 1.0f, false);

  // A clip pixel that is armed but not reached stays dark; once reached it is
  // full-on regardless of smoothing or master brightness.
  if (clipPix && frac > (float)(NEO_N - 1) / NEO_N) {
    int32_t col = zoned ? npZoneAt(1.0f) : flatCol;
    npSet(NEO_N - 1, col, 1.0f, clipFull);
  }
}

/* ---- HSV <-> RGB, for the colour editor -------------------------------- */
static void npToHSV(int32_t c, int *h, int *sv, int *v) {
  float r = npR(c) / 255.0f, g = npG(c) / 255.0f, b = npB(c) / 255.0f;
  float mx = max(r, max(g, b)), mn = min(r, min(g, b));
  float d = mx - mn;
  float hh = 0.0f;
  if (d > 0.0001f) {
    if (mx == r)      hh = 60.0f * fmodf((g - b) / d, 6.0f);
    else if (mx == g) hh = 60.0f * (((b - r) / d) + 2.0f);
    else              hh = 60.0f * (((r - g) / d) + 4.0f);
  }
  if (hh < 0) hh += 360.0f;
  *h  = (int)lroundf(hh) % 360;
  *sv = (int)lroundf((mx <= 0.0001f ? 0.0f : d / mx) * 100.0f);
  *v  = (int)lroundf(mx * 100.0f);
}

static int32_t npFromHSV(int h, int sv, int v) {
  float S = constrain(sv, 0, 100) / 100.0f;
  float V = constrain(v, 0, 100) / 100.0f;
  float H = (float)(((h % 360) + 360) % 360) / 60.0f;
  float C = V * S;
  float X = C * (1.0f - fabsf(fmodf(H, 2.0f) - 1.0f));
  float m = V - C;
  float r = 0, g = 0, b = 0;
  int i = (int)H;
  if (i == 0)      { r = C; g = X; }
  else if (i == 1) { r = X; g = C; }
  else if (i == 2) { g = C; b = X; }
  else if (i == 3) { g = X; b = C; }
  else if (i == 4) { r = X; b = C; }
  else             { r = C; b = X; }
  return npRGB((int)lroundf((r + m) * 255), (int)lroundf((g + m) * 255),
               (int)lroundf((b + m) * 255));
}

/* ###########################################################################
 * #                        ROTARY ENCODER                                   #
 * #                                                                         #
 * #  Interrupt-driven quadrature decode. Both edges of both channels feed a #
 * #  16-entry state table: the index is the previous AB pair concatenated   #
 * #  with the current one, and the table yields -1, 0 or +1. Illegal        #
 * #  transitions (contact bounce) map to 0, so bounce is rejected by the    #
 * #  decoder itself rather than by a debounce delay -- no rotation is ever  #
 * #  missed, however fast the knob is spun.                                 #
 * #                                                                         #
 * #  The encoder is RELATIVE. There is no absolute position, which is what  #
 * #  removes the entire class of sync bugs the servo + pot arrangement had: #
 * #  a remote change simply overwrites the value, and the next detent moves #
 * #  on from wherever it now sits. Nothing has to be reconciled.            #
 * ######################################################################### */

static const int8_t ENC_TABLE[16] = {
   0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0
};

static volatile int32_t  encRaw  = 0;    // quadrature transitions, signed
static volatile uint8_t  encPrev = 0;
static int32_t  encCarry = 0;            // sub-detent remainder
static uint32_t encLastMs = 0;
static int      encLastDir = 0;

static void IRAM_ATTR encISR() {
  uint8_t cur = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  encRaw += ENC_TABLE[((encPrev << 2) | cur) & 0x0F];
  encPrev = cur;
}

static void encBegin() {
  // INPUT_PULLUP regardless of whether the breakout supplies its own.
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  encPrev = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  encRaw = 0;
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);
}

// Discard anything accumulated but not yet consumed. Called on screen entry
// so a spin left over from the previous screen is not applied to this one.
static void knobInputMode();   // defined just below knobResetSteps

static void knobResetSteps() {
  noInterrupts();
  encRaw = 0;
  interrupts();
  encCarry = 0;
  encLastDir = 0;
}

static void knobUpdate() { /* nothing to poll: the ISR does the work */ }

// Screens call this on entry; with an encoder it just drops stale detents.
static void knobInputMode() { knobResetSteps(); }

/* Detents since the last call. Positive is clockwise unless inverted. */
static int knobSteps() {
  noInterrupts();
  int32_t r = encRaw;
  encRaw = 0;
  interrupts();
  if (!r) return 0;
  if (encCfgInvert) r = -r;
  if (sysKnobInvert) r = -r;

  encCarry += r;
  int div = constrain((int)encCfgDiv, 1, 8);
  int steps = (int)(encCarry / div);        // truncates toward zero
  encCarry -= (int32_t)steps * div;
  return steps;
}

/* Optional acceleration for continuous values (fader, volume, character
 * pick). Menus deliberately do not use it: skipping rows feels broken. */
static int knobAccelMul() {
  if (encCfgAccel <= 1) return 1;
  uint32_t now = millis();
  uint32_t gap = now - encLastMs;
  encLastMs = now;
  if (gap >= (uint32_t)encCfgAccelMs) return 1;
  // Scale linearly from 1x at the threshold up to the configured maximum.
  float f = 1.0f - (float)gap / (float)max((int)encCfgAccelMs, 1);
  int m = 1 + (int)lroundf(f * (float)(encCfgAccel - 1));
  return constrain(m, 1, (int)encCfgAccel);
}

static int knobStepsFast() {
  int s = knobSteps();
  if (!s) return 0;
  return s * knobAccelMul();
}

/* ###########################################################################
 * #                       NAVIGATION / APP STACK                            #
 * ######################################################################### */

static const int NAV_MAX = 6;
static const App *gStack[NAV_MAX];
static int gDepth = 0;
static const App *cur() { return gStack[gDepth]; }

static const App *ALL_APPS[40];   // grow with the app list below
static int ALL_APPS_N = 0;
static int appIndex(const App *a) {
  for (int i = 0; i < ALL_APPS_N; i++) if (ALL_APPS[i] == a) return i;
  return -1;
}

static bool gNavDirty = false;

static void navPush(const App *a) {
  if (!a || gDepth + 1 >= NAV_MAX) return;
  if (cur() && cur()->onExit) cur()->onExit();
  gStack[++gDepth] = a;
  if (a->onEnter) a->onEnter();
  gNavDirty = true;
}

static void navBack() {
  if (gDepth == 0) return;
  if (cur() && cur()->onExit) cur()->onExit();
  gDepth--;
  if (cur() && cur()->onEnter) cur()->onEnter();
  gNavDirty = true;
}

static void navReplace(const App *a) {
  if (!a || gDepth == 0) { navPush(a); return; }
  if (cur() && cur()->onExit) cur()->onExit();
  gStack[gDepth] = a;
  if (a->onEnter) a->onEnter();
  gNavDirty = true;
}

static void navHome() {
  while (gDepth > 0) {
    if (cur() && cur()->onExit) cur()->onExit();
    gDepth--;
  }
  if (cur() && cur()->onEnter) cur()->onEnter();
  gNavDirty = true;
}

// Apps with a `launch` hook can demand prerequisites before being entered.
static void launchApp(const App *a) {
  if (!a) return;
  if (a->launch) a->launch();
  else navPush(a);
}

static void navSaveNow() {
  int32_t *slots[4] = { &navSaved0, &navSaved1, &navSaved2, &navSaved3 };
  for (int i = 0; i < 4; i++)
    *slots[i] = (i <= gDepth && i < NAV_MAX) ? appIndex(gStack[i]) : -1;
  nvTouch();
}

/* ###########################################################################
 * #             BUTTONS: debounce, repeat, configurable back combo          #
 * ######################################################################### */

struct BtnState { bool down; uint32_t downMs, lastRepeat; bool pending, suppressed, stuck; };
static BtnState bs[B_COUNT];
static const int BTN_PINS[B_COUNT] = { PIN_BTN_C, PIN_BTN_B, PIN_BTN_A, PIN_ENC_SW };
static bool gComboFired = false;

static uint8_t backMask() {
  switch (sysBackAct) {
    case 1:  return (1 << B_B) | (1 << B_C);
    case 2:  return (1 << B_A) | (1 << B_C);
    case 3:  return (1 << B_A) | (1 << B_B) | (1 << B_C);
    default: return (1 << B_A) | (1 << B_B);
  }
}

static void doBack() {
  if (cur() && cur()->onBack && cur()->onBack()) return;
  navBack();
}

static void dispatch(BtnId b, BtnEv e) {
  batNoteActivity();
  if (cur() && cur()->onButton) cur()->onButton(b, e);
}

// Suppressed buttons are part of an in-flight back gesture, so a hold-to-seek
// must not see them.
static bool btnIsDown(BtnId b) { return bs[b].down && !bs[b].suppressed; }
static bool btnSuppressed(BtnId b) { return bs[b].suppressed; }

/* Generic UI screens treat the shaft click as another select. Apps that give
 * the shaft button its own meaning -- Speaker, Mixer, Text entry, Encoder
 * Test -- handle B_ENC themselves and never call this. */
static bool btnSelect(BtnId b) { return b == B_B || b == B_ENC; }
static uint32_t btnHeldFor(BtnId b) {
  return btnIsDown(b) ? (uint32_t)(millis() - bs[b].downMs) : 0;
}

/* After waking, the button that woke the device is still held down. Swallow
 * input until every button is released, otherwise the wake press also
 * activates whatever happens to sit under it. */
static bool wakeGuardBlocking() {
  if (!gWakeGuard) return false;
  for (int i = 0; i < B_COUNT; i++)
    if (digitalRead(BTN_PINS[i]) == LOW) { gLastActivity = millis(); return true; }
  gWakeGuard = false;
  for (int i = 0; i < B_COUNT; i++) {
    bs[i].down = false;
    bs[i].pending = false;
    bs[i].suppressed = false;
  }
  return false;
}

static void buttonsPoll() {
  for (int i = 0; i < B_COUNT; i++) {
    bool raw = (digitalRead(BTN_PINS[i]) == LOW);
    /* Something leaning on a button for many seconds is pressure, not a
     * deliberate hold. Once past the limit the button is ignored entirely
     * until it comes back up, so a bagged device cannot repeat its way
     * through a settings page. */
    if (raw && bs[i].down && sysStuckSec > 0 && !bs[i].stuck &&
        elapsed(bs[i].downMs, (uint32_t)sysStuckSec * 1000UL))
      bs[i].stuck = true;
    if (!raw) bs[i].stuck = false;
    if (raw && !bs[i].down) {
      bs[i] = { true, millis(), millis(), true, false };
    } else if (!raw && bs[i].down) {
      if (bs[i].pending && !bs[i].suppressed) dispatch((BtnId)i, EV_PRESS);
      bs[i].down = bs[i].pending = bs[i].suppressed = false;
    }
  }

  // Presses are deferred by COMBO_MS so a combo never fires its members first.
  uint8_t mask = backMask(), downMask = 0;
  for (int i = 0; i < B_COUNT; i++) if (bs[i].down) downMask |= (1 << i);

  if ((downMask & mask) == mask && !gComboFired) {
    for (int i = 0; i < B_COUNT; i++)
      if (mask & (1 << i)) { bs[i].pending = false; bs[i].suppressed = true; }
    gComboFired = true;
    doBack();
  }
  if ((downMask & mask) == 0) gComboFired = false;

  for (int i = 0; i < B_COUNT; i++) {
    // Only chord members need to wait; the rest fire at once so navigation
    // stays snappy while A+B still has time to form.
    uint32_t defer = (mask & (1 << i)) ? (uint32_t)sysComboMs : COMBO_MS;
    if (bs[i].stuck) bs[i].pending = false;
    if (bs[i].pending && elapsed(bs[i].downMs, defer)) {
      bs[i].pending = false;
      if (!bs[i].suppressed) dispatch((BtnId)i, EV_PRESS);
    }
    if (bs[i].down && !bs[i].pending && !bs[i].suppressed &&
        sysRepeatDly > 0 && !bs[i].stuck &&
        elapsed(bs[i].downMs, (uint32_t)sysRepeatDly) &&
        elapsed(bs[i].lastRepeat, (uint32_t)sysRepeatRate)) {
      bs[i].lastRepeat = millis();
      dispatch((BtnId)i, EV_REPEAT);
    }
  }
}

/* ###########################################################################
 * #                    CONFIG PAGE ENGINE                                   #
 * #                                                                         #
 * #  One generic driver renders every menu AND every settings page: a menu  #
 * #  is just a page whose rows are all C_LINK. To add a screen, declare a   #
 * #  CfgItem[] plus a CfgPage, then wrap it in a three-line App.            #
 * ######################################################################### */

static CfgPage *gCfg = nullptr;
static const CfgItem *gDropItem = nullptr;

static void applySystemCfg() {
  display.setRotation(sysFlip ? 3 : 1);
  display.setContrast((uint8_t)constrain(sysBright, 1, 255));
}

static void cfgValueText(const CfgItem *it, char *buf, size_t n) {
  buf[0] = 0;
  if (it->fmt) { it->fmt(buf, n); return; }
  switch (it->type) {
    case C_INT:
      snprintf(buf, n, "%ld%s", (long)*it->val, it->unit ? it->unit : "");
      break;
    case C_BOOL:
      snprintf(buf, n, "%s", *it->val ? "On" : "Off");
      break;
    case C_ENUM: {
      int32_t v = constrain(*it->val, 0, (int32_t)it->nopts - 1);
      snprintf(buf, n, "%s", it->opts[v]);
      break;
    }
    default: break;
  }
}

static bool cfgEditable(const CfgItem *it) {
  return it->type == C_INT || it->type == C_BOOL || it->type == C_ENUM;
}

// Long enum labels crowd the row, so those open a full-screen picker instead.
static bool cfgNeedsDropdown(const CfgItem *it) {
  if (it->type != C_ENUM || !it->opts) return false;
  int w = 0;
  for (int i = 0; i < it->nopts; i++) w = max(w, textW(it->opts[i]));
  return (textW(it->label) + w + 14) > ROW_W;
}

static void cfgAdjust(const CfgItem *it, int steps) {
  if (!cfgEditable(it) || !steps) return;
  int32_t v = *it->val;
  if (it->type == C_BOOL) {
    v = (v + steps) & 1;
  } else if (it->type == C_ENUM) {
    int n = it->nopts;
    v = ((v + steps) % n + n) % n;
  } else {
    v = constrain(v + (int32_t)steps * it->step, it->lo, it->hi);
  }
  if (v != *it->val) {
    *it->val = v;
    nvTouch();
    if (it->val == &sysBright || it->val == &sysFlip) applySystemCfg();
  }
}

static void cfgOpen(CfgPage *p) {
  gCfg = p;
  p->editing = false;
  knobInputMode();
}

static void cfgDraw() {
  if (!gCfg) return;
  CfgPage *p = gCfg;
  drawTitle(p->dynTitle ? p->dynTitle() : p->title);

  int first = 0;
  if (p->sel >= ROWS_VIS) first = p->sel - ROWS_VIS + 1;
  if (first > p->n - ROWS_VIS) first = max(0, p->n - ROWS_VIS);

  for (int i = 0; i < ROWS_VIS && first + i < p->n; i++) {
    int idx = first + i, y = ROW_TOP + i * ROW_H;
    const CfgItem *it = &p->items[idx];
    bool sel = (idx == p->sel);
    char vb[24];
    cfgValueText(it, vb, sizeof(vb));
    Glyph tail = (it->type == C_LINK) ? G_RIGHT : G_NONE;
    drawRow(y, sel, sel && p->editing, it->label, vb[0] ? vb : nullptr, tail);
  }
  drawScrollbar(p->n, first);
  drawButtonLabels(G_UP, p->editing ? G_SELECT_OPEN : G_SELECT, G_DOWN);
}

static void cfgMove(int d) {
  if (!gCfg || !gCfg->n) return;
  int s = (int)gCfg->sel + d;
  gCfg->sel = (uint8_t)constrain(s, 0, (int)gCfg->n - 1);
}

static void cfgActivate();   // defined after the dropdown app

static void cfgButton(BtnId b, BtnEv e) {
  if (!gCfg || !gCfg->n) return;
  const CfgItem *it = &gCfg->items[gCfg->sel];
  if (gCfg->editing) {
    if (b == B_C) cfgAdjust(it, +1);
    else if (b == B_A) cfgAdjust(it, -1);
    else if (btnSelect(b) && e == EV_PRESS) gCfg->editing = false;
    return;
  }
  if (b == B_C) cfgMove(-1);
  else if (b == B_A) cfgMove(+1);
  else if (btnSelect(b) && e == EV_PRESS) cfgActivate();
}

static void cfgKnob(int steps) {
  if (!gCfg || !gCfg->n || !steps) return;
  const CfgItem *it = &gCfg->items[gCfg->sel];
  bool edits = gCfg->editing || (sysKnobEdit == 0 && cfgEditable(it));
  if (edits) cfgAdjust(it, steps);
  else cfgMove(steps > 0 ? +1 : -1);
}

/* ---- dropdown picker for enums whose labels won't fit inline ------------- */
static uint8_t gDropSel = 0;

static void dropEnter() { knobInputMode(); gDropSel = gDropItem ? (uint8_t)constrain(*gDropItem->val, 0, (int32_t)gDropItem->nopts - 1) : 0; }

static void dropDraw() {
  if (!gDropItem) return;
  drawTitle(gDropItem->label);
  int n = gDropItem->nopts;
  int first = (gDropSel >= ROWS_VIS) ? gDropSel - ROWS_VIS + 1 : 0;
  for (int i = 0; i < ROWS_VIS && first + i < n; i++) {
    int idx = first + i;
    drawRow(ROW_TOP + i * ROW_H, idx == gDropSel, false,
            gDropItem->opts[idx], nullptr, G_NONE);
  }
  drawScrollbar(n, first);
  drawButtonLabels(G_UP, G_SELECT, G_DOWN);
}

static void dropMove(int d) {
  if (!gDropItem) return;
  gDropSel = (uint8_t)constrain((int)gDropSel + d, 0, (int)gDropItem->nopts - 1);
}

static void dropButton(BtnId b, BtnEv e) {
  if (b == B_C) dropMove(-1);
  else if (b == B_A) dropMove(+1);
  else if (btnSelect(b) && e == EV_PRESS && gDropItem) {
    *gDropItem->val = gDropSel;
    nvTouch();
    applySystemCfg();
    navBack();
  }
}

static void dropKnob(int s) { if (s) dropMove(s > 0 ? +1 : -1); }

static const App AppDropdown = {
  "Select", nullptr, dropEnter, nullptr, nullptr, dropDraw, dropButton, dropKnob,
  nullptr, nullptr
};

static void cfgActivate() {
  const CfgItem *it = &gCfg->items[gCfg->sel];
  switch (it->type) {
    case C_LINK: if (it->target) launchApp(it->target); break;
    case C_ACT:  if (it->action) it->action(); break;
    case C_ENUM:
      if (cfgNeedsDropdown(it)) { gDropItem = it; navPush(&AppDropdown); }
      else gCfg->editing = true;
      break;
    case C_INT:
    case C_BOOL: gCfg->editing = true; break;
    default: break;
  }
}

// Re-entering a config page (e.g. on back) must restore it as the active page.
#define CFG_APP(fnprefix, pagevar)                                            \
  static void fnprefix##Enter() { cfgOpen(&pagevar); }                        \
  static void fnprefix##Exit()  { if (gCfg == &pagevar) pagevar.editing = false; }

/* ###########################################################################
 * #                          TEXT INPUT                                     #
 * #  Knob picks the character, A/C move the cursor, B confirms. A blank     #
 * #  cell that the cursor leaves is deleted. There is always one trailing   #
 * #  blank so the string can grow.                                          #
 * ######################################################################### */

static const char TI_CHARS[] =
  "abcdefghijklmnopqrstuvwxyz"
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "0123456789"
  "-_.:/@#$%&*+=!?,;'\"()[]{}<>|\\^~` ";

static TextReq gTi;
static uint8_t tiIdx[80];
static int tiCells = 1, tiCur = 0;
static bool  tiEditing = false;

static int tiN() { return (int)strlen(TI_CHARS); }
static char tiChar(int i) { return (i <= 0) ? 0 : TI_CHARS[i - 1]; }
/* Past the last character cell sit two buttons, reached by continuing to
 * press right: OK then Clear. Same idea as the IP editor. */
static bool tiOnOk()    { return tiCur == tiCells; }
static bool tiOnClear() { return tiCur == tiCells + 1; }
static bool tiPastEnd() { return tiCur >= tiCells; }

/* With a relative encoder there is nothing to re-arm: moving the cursor no
 * longer risks the knob's resting position overwriting the new cell. Kept as
 * a no-op so every call site does not have to change. */
static void tiArmKnob() { knobResetSteps(); }

// Shaft button toggles the case of the letters being entered.
/* Caps lock is a MODE, not a one-shot remap.
 *
 * TI_CHARS holds both cases back to back, so the old toggle merely jumped to
 * the matching letter in the other half: it changed one character, and the
 * very next detent stepped straight back into the other case. Instead the
 * rotation order is filtered, offering only letters of the active case, so
 * the lock genuinely holds for everything typed afterwards.
 *
 * raw index: 0 = blank, otherwise TI_CHARS offset + 1
 *   TI_CHARS[0..25] lower, [26..51] upper, [52..] digits and symbols      */
static bool tiCaps = false;

static const int TI_LETTERS = 26;
static int tiEffCount() { return 1 + TI_LETTERS + (tiN() - 2 * TI_LETTERS); }

// effective position -> raw index, wrapping at both ends
static int tiEffToRaw(int pos) {
  int n = tiEffCount();
  pos = ((pos % n) + n) % n;
  if (pos == 0) return 0;                                    // blank
  if (pos <= TI_LETTERS) return (tiCaps ? TI_LETTERS : 0) + pos;
  return 2 * TI_LETTERS + (pos - TI_LETTERS);                // digits, symbols
}

// raw index -> effective position; letters fold onto the active case
static int tiRawToEff(int raw) {
  if (raw <= 0) return 0;
  int c = raw - 1;
  if (c < TI_LETTERS) return c + 1;                          // lower
  if (c < 2 * TI_LETTERS) return c - TI_LETTERS + 1;         // upper
  return c - 2 * TI_LETTERS + TI_LETTERS + 1;
}

static void textInputBegin(const char *title, char *buf, uint8_t cap,
                           void (*onDone)()) {
  gTi.title = title; gTi.buf = buf; gTi.cap = cap; gTi.onDone = onDone;
  memset(tiIdx, 0, sizeof(tiIdx));
  tiCells = 1; tiCur = 0; tiEditing = false;
  for (int i = 0; buf[i] && i < cap - 1 && i < (int)sizeof(tiIdx) - 2; i++) {
    const char *p = strchr(TI_CHARS, buf[i]);
    tiIdx[i] = p ? (uint8_t)(p - TI_CHARS + 1) : 0;
    tiCells = i + 2;
  }
  tiIdx[tiCells - 1] = 0;
  tiCur = tiCells - 1;
}

static void tiClearAll() {
  memset(tiIdx, 0, sizeof(tiIdx));
  tiCells = 1;
  tiCur = 0;
  tiEditing = false;
  tiArmKnob();
}

static void tiEnter() { knobInputMode(); tiArmKnob(); }

static void tiGrowIfNeeded() {
  if (tiCur == tiCells - 1 && tiIdx[tiCur] != 0 &&
      tiCells < (int)((gTi.cap < sizeof(tiIdx) ? gTi.cap : sizeof(tiIdx)) - 1)) {
    tiCells++;
    tiIdx[tiCells - 1] = 0;
  }
}

static void tiRotate(int steps) {
  tiIdx[tiCur] = (uint8_t)tiEffToRaw(tiRawToEff(tiIdx[tiCur]) + steps);
  tiGrowIfNeeded();
}

/* Detents step through the character set, wrapping at both ends. Acceleration
 * applies here: a slow turn picks neighbours precisely, a fast spin crosses
 * the alphabet without a long grind. */
static void tiApplyKnob() {
  if (tiPastEnd()) return;
  int s = knobStepsFast();
  if (s) tiRotate(s);
}

/* Flip the mode, then re-map the character under the cursor through the new
 * mapping so the screen matches what further rotation will now produce.
 * Non-letters are unaffected: their effective position is case-independent. */
static void tiToggleCaps() {
  tiCaps = !tiCaps;
  if (tiPastEnd()) return;
  int raw = tiIdx[tiCur];
  if (raw <= 0 || raw - 1 >= 2 * TI_LETTERS) return;
  tiIdx[tiCur] = (uint8_t)tiEffToRaw(tiRawToEff(raw));
}

static void tiDeleteAt(int c) {
  for (int i = c; i < tiCells - 1; i++) tiIdx[i] = tiIdx[i + 1];
  if (tiCells > 1) tiCells--;
}

static void tiMove(int d) {
  if (tiPastEnd()) {
    if (d > 0) { if (tiOnOk()) tiCur = tiCells + 1; }     // OK -> Clear
    else       { tiCur = tiOnClear() ? tiCells : tiCells - 1; }
    tiArmKnob();
    return;
  }
  bool blank = (tiIdx[tiCur] == 0) && (tiCur < tiCells - 1);
  if (d > 0) {
    if (blank) tiDeleteAt(tiCur);              // leaving a blank deletes it
    else if (tiCur < tiCells - 1) tiCur++;
    else tiCur = tiCells;                      // past the last cell is OK
  } else {
    if (blank) { tiDeleteAt(tiCur); if (tiCur > 0) tiCur--; }
    else if (tiCur > 0) tiCur--;
  }
  tiArmKnob();
}

static void tiCommit() {
  int o = 0;
  for (int i = 0; i < tiCells && o < gTi.cap - 1; i++) {
    char c = tiChar(tiIdx[i]);
    if (c) gTi.buf[o++] = c;
  }
  gTi.buf[o] = 0;
}

static void tiDraw() {
  drawTitle(gTi.title);

  const int cw = 7, y = 20, vis = CONTENT_W / cw;
  int anchor = min(tiCur, tiCells - 1);
  int first = (anchor >= vis) ? anchor - vis + 1 : 0;

  for (int i = 0; i < vis && first + i < tiCells; i++) {
    int idx = first + i, x = i * cw;
    bool cursor = (idx == tiCur);
    if (cursor) display.fillRect(x, y - 1, cw, 11, SH110X_WHITE);
    char c = tiChar(tiIdx[idx]);
    uint16_t fg = cursor ? SH110X_BLACK : SH110X_WHITE;
    display.setTextColor(fg);
    if (c == ' ') display.drawFastHLine(x + 1, y + 7, 5, fg);
    else if (c)  { display.setCursor(x + 1, y + 1); display.print(c); }
    display.setTextColor(SH110X_WHITE);
    if (!cursor) display.drawFastHLine(x, y + 10, cw - 1, SH110X_WHITE);
    // clicked into this cell: chevrons show A/C now cycle the character
    if (cursor && tiEditing) {
      drawGlyph(x + cw / 2, y - 5, G_UP, SH110X_WHITE);
      drawGlyph(x + cw / 2, y + 15, G_DOWN, SH110X_WHITE);
    }
  }

  int used = 0;
  for (int i = 0; i < tiCells; i++) if (tiIdx[i]) used++;
  display.setCursor(0, 40);
  if (tiPastEnd()) display.printf("%d/%d chars", used, gTi.cap - 1);
  else {
    char c = tiChar(tiIdx[tiCur]);
    if (!c)           display.printf("%d/%d  [blank]", used, gTi.cap - 1);
    else if (c == ' ')display.printf("%d/%d  [space]", used, gTi.cap - 1);
    else              display.printf("%d/%d  '%c'", used, gTi.cap - 1, c);
  }

  // OK and Clear, bottom right of the content area
  const int btnH = 14, btnY = 48;
  const int okW = 26, clW = 38;
  const int okX = CONTENT_W - okW - 1;
  const int clX = okX - clW - 4;
  struct { int x, w; const char *l; bool sel; } tb[2] = {
    { clX, clW, "Clear", tiOnClear() },
    { okX, okW, "OK",    tiOnOk()    },
  };
  for (int i = 0; i < 2; i++) {
    if (tb[i].sel) {
      display.fillRoundRect(tb[i].x, btnY, tb[i].w, btnH, 4, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
    } else {
      display.drawRoundRect(tb[i].x, btnY, tb[i].w, btnH, 4, SH110X_WHITE);
      display.setTextColor(SH110X_WHITE);
    }
    display.setCursor(tb[i].x + (tb[i].w - (int)strlen(tb[i].l) * 6) / 2, btnY + 4);
    display.print(tb[i].l);
    display.setTextColor(SH110X_WHITE);
  }

  if (encCfgTextBtn == 0 && !tiPastEnd()) {   // shaft click toggles case
    display.setCursor(0, 52);
    display.print(tiCaps ? "ABC" : "abc");
  }

  if (tiEditing)        drawButtonLabels(G_UP, G_SELECT_OPEN, G_DOWN);
  else if (tiOnClear()) drawButtonLabels(G_NONE, G_SELECT, G_LEFT);
  else                  drawButtonLabels(G_RIGHT, G_SELECT, G_LEFT);
}

static void tiButton(BtnId b, BtnEv e) {
  if (tiEditing) {                       // A/C cycle the character
    if (b == B_C) tiRotate(+1);
    else if (b == B_A) tiRotate(-1);
    else if (b == B_B && e == EV_PRESS) tiEditing = false;
    return;
  }
  if (b == B_C) tiMove(+1);
  else if (b == B_A) tiMove(-1);
  else if (b == B_B && e == EV_PRESS) {
    if (tiOnClear()) tiClearAll();
    else if (tiOnOk()) { tiCommit(); if (gTi.onDone) gTi.onDone(); }
    else tiEditing = true;
  } else if (b == B_ENC && e == EV_PRESS) {
    // Shaft click follows the selection when it is on a button, so the
    // encoder can drive the whole screen without reaching for B.
    if (tiOnClear()) tiClearAll();
    else if (tiOnOk()) { tiCommit(); if (gTi.onDone) gTi.onDone(); }
    else if (encCfgTextBtn == 0) tiToggleCaps();
    else if (encCfgTextBtn == 1) { tiCommit(); if (gTi.onDone) gTi.onDone(); }
  }
}

static void tiTick() { tiApplyKnob(); }

static const App AppTextInput = {
  "Text Input", nullptr, tiEnter, nullptr, tiTick, tiDraw, tiButton, nullptr,
  nullptr, nullptr
};

/* ###########################################################################
 * #                            IP INPUT                                     #
 * #  Twelve digit cells shown as ___.___.___.___ ; the cursor skips dots.   #
 * #  Knob cycles blank,0..9. Invalid entries are rejected and clear the IP. #
 * ######################################################################### */

static int8_t ipD[12];
static int ipCur = 0;
static int32_t *gIpTarget = nullptr;
static const char *gIpTitle = "IP Address";
static void (*gIpDone)() = nullptr;
static bool gIpError = false;

static void ipInputBegin(int32_t *target, const char *title, void (*onDone)()) {
  gIpTarget = target; gIpTitle = title; ipField = 0;
  uint32_t packed = target ? (uint32_t)*target : 0;
  gIpDone = onDone; gIpError = false; ipCur = 0;
  for (int i = 0; i < 12; i++) ipD[i] = -1;
  if (packed) {
    for (int o = 0; o < 4; o++) {
      int v = (packed >> (8 * o)) & 0xFF;     // IPAddress packs LSB = first octet
      ipD[o * 3 + 0] = (v / 100) % 10;
      ipD[o * 3 + 1] = (v / 10) % 10;
      ipD[o * 3 + 2] = v % 10;
    }
  }
}

static void ipArmKnob() { knobResetSteps(); }
static void ipEnter() { knobInputMode(); ipArmKnob(); }

// Detents cycle blank, 0..9 and wrap. No acceleration: eleven values is a
// short list and precision matters more than speed.
static void ipApplyKnob() {
  if (gIpError || ipField != 0) return;   // knob edits digits only
  int st = knobSteps();
  if (!st) return;
  int v = (int)ipD[ipCur] + 1 + st;       // 0 = blank, 1..10 = 0..9
  v %= 11;
  if (v < 0) v += 11;
  ipD[ipCur] = (int8_t)(v - 1);
}
static void ipTick() { ipApplyKnob(); }

static bool ipParse(uint32_t *out) {
  uint32_t packed = 0;
  for (int o = 0; o < 4; o++) {
    int firstD = -1, lastD = -1;
    for (int k = 0; k < 3; k++) {
      if (ipD[o * 3 + k] >= 0) { if (firstD < 0) firstD = k; lastD = k; }
    }
    if (firstD < 0) return false;                       // empty octet
    for (int k = firstD; k <= lastD; k++)
      if (ipD[o * 3 + k] < 0) return false;             // gap inside the octet
    int v = 0;
    for (int k = firstD; k <= lastD; k++) v = v * 10 + ipD[o * 3 + k];
    if (v > 255) return false;
    packed |= ((uint32_t)v) << (8 * o);
  }
  *out = packed;
  return true;
}

static void ipDraw() {
  if (gIpError) {
    uiWide();
    drawTitle("Invalid IP");
    display.setCursor(0, 20);
    display.print("Octets must be");
    display.setCursor(0, 30);
    display.print("0-255 with no");
    display.setCursor(0, 40);
    display.print("gaps. IP cleared.");
    display.setCursor(0, 54);
    display.print("back to exit");
    return;
  }

  drawTitle(gIpTitle);
  const int cw = 7, y = 24;
  for (int c = 0; c < 15; c++) {
    int x = c * cw;
    if (c % 4 == 3) { display.setCursor(x + 2, y + 1); display.print('.'); continue; }
    int d = c - c / 4;
    bool cursor = (d == ipCur) && ipField == 0;
    if (cursor) display.fillRect(x, y - 1, cw, 11, SH110X_WHITE);
    display.setTextColor(cursor ? SH110X_BLACK : SH110X_WHITE);
    if (ipD[d] >= 0) { display.setCursor(x + 1, y + 1); display.print((char)('0' + ipD[d])); }
    display.setTextColor(SH110X_WHITE);
    if (!cursor) display.drawFastHLine(x, y + 10, cw - 1, SH110X_WHITE);
  }
  /* Demo and Clear sit past the last digit: keep pressing right to reach
   * them. Only the mixer offers Demo. */
  const int by = 40, bh = 13;
  const char *lbl[2]; int bw[2]; int nb = 0;
  if (gIpAllowDemo) { lbl[nb] = "Demo"; bw[nb] = 34; nb++; }
  lbl[nb] = "Clear"; bw[nb] = 40; nb++;

  int bx = 0;
  for (int i = 0; i < nb; i++) {
    int field = gIpAllowDemo ? i + 1 : 2;
    bool sel = (ipField == field);
    if (sel) {
      display.fillRoundRect(bx, by, bw[i], bh, 3, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
    } else {
      display.drawRoundRect(bx, by, bw[i], bh, 3, SH110X_WHITE);
      display.setTextColor(SH110X_WHITE);
    }
    display.setCursor(bx + (bw[i] - (int)strlen(lbl[i]) * 6) / 2, by + 4);
    display.print(lbl[i]);
    display.setTextColor(SH110X_WHITE);
    bx += bw[i] + 4;
  }

  display.setCursor(0, 56);
  if (ipField == 0)      display.print("click confirms");
  else if (ipField == 1) display.print("run without mixer");
  else                   display.print("erase the IP");

  drawButtonLabels(G_RIGHT, G_SELECT, G_LEFT);
}

static void ipCommit() {
  uint32_t packed;
  if (ipParse(&packed)) {
    if (gIpTarget) *gIpTarget = (int32_t)packed;
    nvTouch(); nvFlush();
    if (gIpDone) gIpDone(); else navBack();
  } else {
    if (gIpTarget) *gIpTarget = 0;
    nvTouch(); nvFlush();
    gIpError = true;
  }
}

static void ipClear() {
  for (int i = 0; i < 12; i++) ipD[i] = -1;
  if (gIpTarget) *gIpTarget = 0;
  nvTouch(); nvFlush();
  ipCur = 0; ipField = 0;
  ipArmKnob();
}

static void ipButton(BtnId b, BtnEv e) {
  if (gIpError) return;
  // Buttons live past the last digit: keep moving right to reach them.
  if (b == B_C) {
    if (ipField == 0 && ipCur < 11) ipCur++;
    else if (ipField == 0) ipField = gIpAllowDemo ? 1 : 2;
    else if (ipField < 2) ipField++;
    ipArmKnob();
  } else if (b == B_A) {
    if (ipField > 0) {
      if (gIpAllowDemo && ipField == 2) ipField = 1;
      else { ipField = 0; ipCur = 11; }
    } else if (ipCur > 0) ipCur--;
    ipArmKnob();
  } else if (btnSelect(b) && e == EV_PRESS) {
    if (ipField == 0) ipCommit();
    else if (ipField == 1) { if (gIpDemo) gIpDemo(); }
    else ipClear();
  }
}


static const App AppIpInput = {
  "IP Address", nullptr, ipEnter, nullptr, ipTick, ipDraw, ipButton, nullptr,
  nullptr, nullptr
};

/* ###########################################################################
 * #                           WI-FI APPS                                    #
 * ######################################################################### */

static const App *gAfterConnect = nullptr;   // resume target once prereqs are met
static bool gWifiGateway = false;            // Wi-Fi page entered as a gate
static bool gSpkGateway  = false;            // Speaker page entered as a gate

static bool wifiConfigured() { return wifiSsid[0] != 0; }
static bool wifiOnline()     { return WiFi.status() == WL_CONNECTED; }

static const char *wifiTitle() {
  if (wifiOnline())      return "Wi-Fi: Connected";
  if (!wifiConfigured()) return "Wi-Fi: Setup";
  return "Wi-Fi: Offline";
}

/* ---- connecting / loading screen ---------------------------------------- */
static uint32_t wcStart = 0;
static bool wcFailed = false;

static void wcEnter() {
  wcStart = millis();
  wcFailed = false;
  WiFi.disconnect(false, true);
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid, wifiPass);
  wifiTune();
}

static void wcTick() {
  if (wcFailed) return;
  if (wifiOnline()) {
    nvNetRemember(wifiSsid, wifiPass);
    nvFlush();
    if (gAfterConnect) {
      const App *t = gAfterConnect;
      gAfterConnect = nullptr;
      gWifiGateway = false;
      navHome();
      launchApp(t);
    } else navBack();
    return;
  }
  if (elapsed(wcStart, WIFI_TIMEOUT_MS)) wcFailed = true;
}

static void wcDraw() {
  uiWide();
  drawTitle(wcFailed ? "Connection Failed" : "Connecting...");
  display.setCursor(0, 20);
  printClipped(0, 20, wifiSsid, CONTENT_W);
  if (wcFailed) {
    display.setCursor(0, 34);
    display.print("Check password");
    display.setCursor(0, 44);
    display.print("or signal.");
    display.setCursor(0, 56);
    display.print("back to exit");
  } else {
    display.setCursor(0, 36);
    display.printf("%lus / %lus", (unsigned long)((millis() - wcStart) / 1000),
                   (unsigned long)(WIFI_TIMEOUT_MS / 1000));
    drawBar(0, 48, CONTENT_W, 7, (float)(millis() - wcStart) / WIFI_TIMEOUT_MS);
  }
}

static const App AppWifiConnect = {
  "Connecting", nullptr, wcEnter, nullptr, wcTick, wcDraw, nullptr, nullptr,
  nullptr, nullptr
};

/* ---- password step shared by both entry paths --------------------------- */
static void wifiGoConnect() { navReplace(&AppWifiConnect); }

static void wifiAskPassword() {
  textInputBegin("Wi-Fi Password", wifiPass, sizeof(wifiPass), wifiGoConnect);
  navReplace(&AppTextInput);
}

static void wifiAfterSsid() {
  if (!wifiSsid[0]) { navBack(); return; }
  wifiAskPassword();
}

static void wifiManualStart() {
  textInputBegin("Wi-Fi SSID", wifiSsid, sizeof(wifiSsid), wifiAfterSsid);
  navPush(&AppTextInput);
}

/* ---- scan --------------------------------------------------------------- */
static int wsCount = -1;      // -1 scanning, >=0 results
static int wsSel = 0;

static uint32_t wsStart = 0;
static int      wsRetries = 0;
static bool     wsFailed = false;

static bool wsWaiting = false;      // waiting out the gap before a retry

static void wsBeginScan() {
  if (WiFi.getMode() != WIFI_STA && WiFi.getMode() != WIFI_AP_STA)
    WiFi.mode(WIFI_STA);            // only if needed: mode() resets the radio
  WiFi.scanDelete();
  // show_hidden = true: APs that suppress their SSID still appear, which is
  // otherwise indistinguishable from "no networks here".
  int r = WiFi.scanNetworks(true, true);
  wsStart = millis();
  // A synchronous rejection means the radio is busy right now; wait rather
  // than burning a retry immediately.
  wsWaiting = (r == WIFI_SCAN_FAILED);
}

static void wsEnter() {
  knobInputMode();
  wsCount = -1;
  wsSel = 0;
  wsRetries = 0;
  wsFailed = false;
  wsWaiting = false;
  wsBeginScan();
}

static void wsExit() { WiFi.scanDelete(); }

/* scanComplete() returns -1 while running and -2 on failure. The old code
 * accepted only n >= 0, so a failed scan left the screen saying "Scanning"
 * forever -- and a scan started while the radio is busy associating or
 * reconnecting fails routinely, which is exactly why this behaved
 * differently in different places. Failures and stalls now retry, then give
 * up visibly instead of silently. */
/* scanComplete() returns -1 while running and -2 on failure -- and also -2
 * when no scan is running at all, which is what a rejected start looks like.
 * The previous version retried the instant it saw -2, so three retries were
 * spent inside a few hundred milliseconds and it reported failure before the
 * radio had any chance to become free. Retries are now spaced out. */
static void wsTick() {
  if (wsCount >= 0 || wsFailed) return;

  if (wsWaiting) {                      // hold off, then start again
    if (!elapsed(wsStart, 1200)) return;
    wsWaiting = false;
    wsRetries++;
    if (wsRetries > 6) { wsFailed = true; return; }
    wsBeginScan();
    return;
  }

  int n = WiFi.scanComplete();
  if (n >= 0) { wsCount = n; return; }

  bool bad = (n == WIFI_SCAN_FAILED) || elapsed(wsStart, 10000);
  if (!bad) return;
  if (wsRetries > 6) { wsFailed = true; return; }
  wsWaiting = true;                     // wsTick above will restart it
  wsStart = millis();
}

static void drawSignalBars(int x, int y, int rssi) {
  int lvl = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    if (i < lvl) display.fillRect(x + i * 4, y + 9 - h, 3, h, SH110X_WHITE);
    else         display.drawFastHLine(x + i * 4, y + 8, 3, SH110X_WHITE);
  }
}

static void drawLock(int x, int y) {
  display.fillRect(x, y + 4, 5, 4, SH110X_WHITE);
  display.drawFastHLine(x + 1, y + 2, 3, SH110X_WHITE);
  display.drawPixel(x, y + 3, SH110X_WHITE);
  display.drawPixel(x + 4, y + 3, SH110X_WHITE);
}

static void wsDraw() {
  if (wsCount <= 0) uiWide();
  drawTitle("Scan");
  if (wsFailed) {
    display.setCursor(0, 22);
    display.print("Scan failed");
    display.setCursor(0, 36);
    display.print("Radio busy. Try");
    display.setCursor(0, 46);
    display.print("again, or Manual.");
    drawButtonLabels(G_NONE, G_SELECT, G_NONE);
    return;
  }
  if (wsCount < 0) {
    display.setCursor(0, 28);
    display.printf("Scanning...%s", wsRetries ? " (retry)" : "");
    if (wsRetries) { display.setCursor(0, 38); display.printf("attempt %d/7", wsRetries + 1); }
    drawBar(0, 44, CONTENT_W, 6,
            (float)(millis() - wsStart) / 12000.0f);
    return;
  }
  if (wsCount == 0) {
    display.setCursor(0, 28);
    display.print("No networks found");
    display.setCursor(0, 44);
    display.print("back to exit");
    return;
  }

  int first = (wsSel >= ROWS_VIS) ? wsSel - ROWS_VIS + 1 : 0;
  for (int i = 0; i < ROWS_VIS && first + i < wsCount; i++) {
    int idx = first + i, y = ROW_TOP + i * ROW_H;
    bool sel = (idx == wsSel);
    if (sel) display.fillRect(0, y, ROW_W, ROW_H - 1, SH110X_WHITE);
    display.setTextColor(sel ? SH110X_BLACK : SH110X_WHITE);
    printClipped(3, y + 2, WiFi.SSID(idx).c_str(), ROW_W - 32);
    display.setTextColor(SH110X_WHITE);
    if (sel) {
      // punch the indicators out of the highlight so they stay readable
      display.fillRect(ROW_W - 27, y, 27, ROW_H - 1, SH110X_BLACK);
    }
    if (WiFi.encryptionType(idx) != WIFI_AUTH_OPEN) drawLock(ROW_W - 25, y + 1);
    drawSignalBars(ROW_W - 17, y + 1, WiFi.RSSI(idx));
  }
  drawScrollbar(wsCount, first);
  drawButtonLabels(G_UP, G_SELECT, G_DOWN);
}

static void wsMove(int d) {
  if (wsCount <= 0) return;
  wsSel = constrain(wsSel + d, 0, wsCount - 1);
}

static void wsSelect() {
  if (wsCount <= 0) return;
  snprintf(wifiSsid, sizeof(wifiSsid), "%s", WiFi.SSID(wsSel).c_str());
  bool open = (WiFi.encryptionType(wsSel) == WIFI_AUTH_OPEN);
  if (open) { wifiPass[0] = 0; navReplace(&AppWifiConnect); }
  else      wifiAskPassword();          // replaces the scan screen
}

static void wsButton(BtnId b, BtnEv e) {
  if (wsCount < 0) return;
  if (b == B_C) wsMove(-1);
  else if (b == B_A) wsMove(+1);
  else if (btnSelect(b) && e == EV_PRESS) {
    if (wsFailed) { wsFailed = false; wsRetries = 0; wsWaiting = false;
                    wsCount = -1; wsBeginScan(); }
    else wsSelect();
  }
}

static void wsKnob(int s) { if (s) wsMove(s > 0 ? +1 : -1); }

static const App AppWifiScan = {
  "Scan", nullptr, wsEnter, wsExit, wsTick, wsDraw, wsButton, wsKnob,
  nullptr, nullptr
};

/* ---- Wi-Fi info: current connection, then saved history ----------------- */
static int wiMode = 0;      // 0 current, 1 list, 2 detail
static int wiSel = 0, wiCount = 0;
static char wiSsid[33], wiPass[65];

static void wiEnter() {
  knobInputMode();
  wiMode = 0;
  wiSel = 0;
  wiCount = nvNetCount();
}

static void wiDrawKV(int y, const char *k, const char *v) {
  display.setCursor(0, y);
  display.print(k);
  printClipped(textW(k) + 6, y, v, CONTENT_W - textW(k) - 6);
}

static void wiDrawCurrent() {
  drawTitle("Current Connection");
  if (!wifiOnline()) {
    display.setCursor(0, 20);
    display.print("No Wi-Fi connected");
    display.setCursor(0, 34);
    display.printf("%d saved network%s", wiCount, wiCount == 1 ? "" : "s");
    display.setCursor(0, 48);
    display.print("press v for list");
    return;
  }
  char buf[32];
  wiDrawKV(14, "SSID", WiFi.SSID().c_str());
  wiDrawKV(23, "PASS", wifiPass[0] ? wifiPass : "(open)");
  wiDrawKV(32, "IP", WiFi.localIP().toString().c_str());
  snprintf(buf, sizeof(buf), "%d dBm  ch%d", (int)WiFi.RSSI(), (int)WiFi.channel());
  wiDrawKV(41, "RF", buf);
  wiDrawKV(50, "BSSID", WiFi.BSSIDstr().c_str());
}

static void wiDrawList() {
  drawTitle("Previous Connections");
  if (wiCount == 0) {
    display.setCursor(0, 28);
    display.print("None saved");
    return;
  }
  int first = (wiSel >= ROWS_VIS) ? wiSel - ROWS_VIS + 1 : 0;
  for (int i = 0; i < ROWS_VIS && first + i < wiCount; i++) {
    int idx = first + i;
    char s[33];
    s[0] = 0;
    nvNetGet(idx, s, sizeof(s), nullptr, 0);
    drawRow(ROW_TOP + i * ROW_H, idx == wiSel, false, s, nullptr, G_RIGHT);
  }
  drawScrollbar(wiCount, first);
}

static void wiDrawDetail() {
  drawTitle(wiSsid[0] ? wiSsid : "Saved Network");
  wiDrawKV(16, "SSID", wiSsid);
  wiDrawKV(26, "PASS", wiPass[0] ? wiPass : "(open)");

  const int bw = 54, bh = 14, bx = 0, by = 40;
  display.fillRoundRect(bx, by, bw, bh, 4, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setCursor(bx + 8, by + 4);
  display.print("Connect");
  display.setTextColor(SH110X_WHITE);

  display.setCursor(0, 57);
  display.print("back to return");
}

static void wiDraw() {
  if (wiMode == 0) wiDrawCurrent();
  else if (wiMode == 1) wiDrawList();
  else wiDrawDetail();
  if (wiMode == 2) drawButtonLabels(G_NONE, G_SELECT, G_NONE);
  else drawButtonLabels(G_UP, G_SELECT, G_DOWN);
}

static void wiMove(int d) {
  if (wiMode == 2) return;
  if (wiMode == 0) {
    // scrolling down off the current-connection page enters the history list
    if (d > 0 && wiCount > 0) { wiMode = 1; wiSel = 0; }
    return;
  }
  int s = wiSel + d;
  if (s < 0) { wiMode = 0; return; }        // scrolling up returns to current
  wiSel = constrain(s, 0, max(0, wiCount - 1));
}

static void wiButton(BtnId b, BtnEv e) {
  if (b == B_C) wiMove(-1);
  else if (b == B_A) wiMove(+1);
  else if (btnSelect(b) && e == EV_PRESS && wiMode == 1 && wiCount > 0) {
    wiSsid[0] = wiPass[0] = 0;
    nvNetGet(wiSel, wiSsid, sizeof(wiSsid), wiPass, sizeof(wiPass));
    wiMode = 2;
  } else if (btnSelect(b) && e == EV_PRESS && wiMode == 2 && wiSsid[0]) {
    // Reconnect to a remembered network without retyping the password.
    snprintf(wifiSsid, sizeof(wifiSsid), "%s", wiSsid);
    snprintf(wifiPass, sizeof(wifiPass), "%s", wiPass);
    nvTouch();
    navReplace(&AppWifiConnect);
  }
}

static void wiKnob(int s) { if (s) wiMove(s > 0 ? +1 : -1); }

static bool wiBack() {
  if (wiMode == 2) { wiMode = 1; return true; }   // detail -> list
  return false;                                    // list/current -> leave app
}

static const App AppWifiInfo = {
  "Wi-Fi info", nullptr, wiEnter, nullptr, nullptr, wiDraw, wiButton, wiKnob,
  nullptr, wiBack
};

/* ---- Wi-Fi menu --------------------------------------------------------- */
static void wifiScanLink() { navPush(&AppWifiScan); }

static const CfgItem WIFI_ITEMS[] = {
  ITEM_ACT ("Scan...",         wifiScanLink),
  ITEM_ACT ("Manual input...", wifiManualStart),
  ITEM_LINK("Wi-Fi info",      AppWifiInfo),
};
static CfgPage PageWifi = { "Wi-Fi", WIFI_ITEMS, 3, 0, false, wifiTitle };

static void wfEnter() { cfgOpen(&PageWifi); }

// When Wi-Fi was entered as a prerequisite for another app, backing out
// abandons that intent and returns home rather than to Settings.
static bool wfBack() {
  if (gWifiGateway) {
    gWifiGateway = false;
    gAfterConnect = nullptr;
    navHome();
    return true;
  }
  return false;
}

static const App AppWifi = {
  "Wi-Fi", nullptr, wfEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, wfBack
};

/* ###########################################################################
 * #                    MIXER LINK  (OSC to the console)                     #
 * ######################################################################### */

/* /meters/0 returns 70 values:
 *   [ 0..31] 32 input channels     [32..39]  8 aux returns
 *   [40..47]  8 fx returns         [48..63] 16 bus masters
 *   [64..69]  6 matrixes
 * Main LR / mono and DCAs are absent, so those strips show no meter.
 * X32/M32 sends floats here; the X-Air family sends int16 dBFS*256 instead. */
struct Group {
  const char *label; const char *pathFmt;
  uint8_t count; int16_t meterBase;
  bool numbered, dcaStyle, enabled;
};

static const Group GROUPS[] = {
  { "CH",   "/ch/%02d",      32,  0, true,  false, true },
  { "AUX",  "/auxin/%02d",    8, 32, true,  false, true },
  { "FX",   "/fxrtn/%02d",    8, 40, true,  false, true },
  { "BUS",  "/bus/%02d",     16, 48, true,  false, true },
  { "MTX",  "/mtx/%02d",      6, 64, true,  false, true },
  { "MAIN", "/main/st",       1, -1, false, false, true },
  { "M/C",  "/main/m",        1, -1, false, false, true },
  { "DCA",  "/dca/%d",        8, -1, true,  true,  true },
};
static const int NUM_GROUPS = sizeof(GROUPS) / sizeof(GROUPS[0]);

static int   mlGroup = 0, mlNum = 1;
static char  mlFaderPath[40], mlNamePath[40];
static char  mlName[16] = "";
static float mlFader = 0.0f;
static uint32_t mlFaderSeq = 0;
static float    mlMeter = 0.0f, mlPeak = 0.0f;
static uint32_t mlPeakMs = 0;
static bool     mlMeterValid = false, mlMetersActive = false, mlUdpUp = false;
/* Demo drives the real Mixer UI from a local model of a console: the same
 * screen, strips, servo sync and de-oscillation, with the network stubbed.
 * It is intercepted at the three points where the app meets the wire --
 * udpTx, mlLinkOk and mlTick -- so the UI code needs no demo branches. */
static bool     mlDemo = false;
static float    mlDemoFader[NUM_GROUPS][32];
static bool     mlDemoInit = false;
static uint32_t mlDemoMs = 0;
static uint32_t mlLastRx = 0, mlLastXremote = 0, mlLastMeter = 0, mlLastResync = 0;
static uint8_t  mlRx[1024], mlTx[256];

static int oscPutStr(uint8_t *b, int off, const char *s) {
  int n = strlen(s);
  memcpy(b + off, s, n); off += n;
  do { b[off++] = 0; } while (off & 3);
  return off;
}
static int oscPutI32(uint8_t *b, int off, int32_t v) {
  b[off++] = (v >> 24) & 0xFF; b[off++] = (v >> 16) & 0xFF;
  b[off++] = (v >> 8) & 0xFF;  b[off++] = v & 0xFF;
  return off;
}
static int oscPutF32(uint8_t *b, int off, float f) {
  uint32_t v; memcpy(&v, &f, 4); return oscPutI32(b, off, (int32_t)v);
}
static int32_t oscI32BE(const uint8_t *b, int o) {
  return ((int32_t)b[o] << 24) | ((int32_t)b[o+1] << 16) |
         ((int32_t)b[o+2] << 8) | (int32_t)b[o+3];
}
static float oscF32BE(const uint8_t *b, int o) {
  uint32_t v = (uint32_t)oscI32BE(b, o); float f; memcpy(&f, &v, 4); return f;
}
static int32_t oscI32LE(const uint8_t *b, int o) {
  return (int32_t)((uint32_t)b[o] | ((uint32_t)b[o+1] << 8) |
                   ((uint32_t)b[o+2] << 16) | ((uint32_t)b[o+3] << 24));
}
static float oscF32LE(const uint8_t *b, int o) {
  uint32_t v = (uint32_t)oscI32LE(b, o); float f; memcpy(&f, &v, 4); return f;
}
static int16_t oscI16LE(const uint8_t *b, int o) {
  return (int16_t)((uint16_t)b[o] | ((uint16_t)b[o+1] << 8));
}

static IPAddress mixerIp() { return IPAddress((uint32_t)mxCfgIp); }
static bool mixerIpSet() { return mxCfgIp != 0; }

static void udpTx(int len) {
  if (mlDemo) return;                    // nothing goes on the wire in demo
  if (!mlUdpUp || !mixerIpSet()) return;
  udp.beginPacket(mixerIp(), (uint16_t)mxCfgPort);
  udp.write(mlTx, len);
  udp.endPacket();
}
static void oscQuery(const char *a) {
  int o = oscPutStr(mlTx, 0, a); o = oscPutStr(mlTx, o, ","); udpTx(o);
}
static void oscFloat(const char *a, float f) {
  int o = oscPutStr(mlTx, 0, a); o = oscPutStr(mlTx, o, ",f");
  o = oscPutF32(mlTx, o, f); udpTx(o);
}
static void oscXremote() { udpTx(oscPutStr(mlTx, 0, "/xremote")); }
static void oscMeterSub() {
  int o = oscPutStr(mlTx, 0, "/meters"); o = oscPutStr(mlTx, o, ",si");
  o = oscPutStr(mlTx, o, "/meters/0"); o = oscPutI32(mlTx, o, mxCfgMeterRate);
  udpTx(o);
}

static void mlBuildPaths() {
  const Group &g = GROUPS[mlGroup];
  char base[32];
  if (g.numbered) snprintf(base, sizeof(base), g.pathFmt, mlNum);
  else            snprintf(base, sizeof(base), "%s", g.pathFmt);
  snprintf(mlFaderPath, sizeof(mlFaderPath), "%s%s", base,
           g.dcaStyle ? "/fader" : "/mix/fader");
  snprintf(mlNamePath, sizeof(mlNamePath), "%s/config/name", base);
}

static void mlDemoLoadStrip() {
  const Group &g = GROUPS[mlGroup];
  int idx = g.numbered ? (mlNum - 1) : 0;
  mlFader = mlDemoFader[mlGroup][constrain(idx, 0, 31)];
  mlFaderSeq++;
  if (g.numbered) snprintf(mlName, sizeof(mlName), "%s %d", g.label, mlNum);
  else            snprintf(mlName, sizeof(mlName), "%s", g.label);
  mlMeterValid = (g.meterBase >= 0);
  mlLastRx = millis();
}

static void mlRequestStrip() {
  if (mlDemo) { mlDemoLoadStrip(); return; }
  mlName[0] = 0; mlMeterValid = false;
  oscQuery(mlFaderPath);
  delay(4);
  oscQuery(mlNamePath);
}

static void mlStripStep(int d) {
  const Group *g = &GROUPS[mlGroup];
  int n = mlNum + d;
  while (n < 1) {
    do { mlGroup = (mlGroup - 1 + NUM_GROUPS) % NUM_GROUPS; }
    while (!GROUPS[mlGroup].enabled);
    g = &GROUPS[mlGroup]; n += g->count;
  }
  while (n > g->count) {
    n -= g->count;
    do { mlGroup = (mlGroup + 1) % NUM_GROUPS; } while (!GROUPS[mlGroup].enabled);
    g = &GROUPS[mlGroup];
  }
  mlNum = n; mlBuildPaths(); mlRequestStrip();
}

static void mlGroupStep() {
  do { mlGroup = (mlGroup + 1) % NUM_GROUPS; } while (!GROUPS[mlGroup].enabled);
  mlNum = 1; mlBuildPaths(); mlRequestStrip();
}

static void mlStripLabel(char *out, size_t n) {
  const Group &g = GROUPS[mlGroup];
  if (g.numbered) snprintf(out, n, "%s%d", g.label, mlNum);
  else            snprintf(out, n, "%s", g.label);
}


static void mlDemoBegin() {
  mlDemo = true;
  if (!mlDemoInit) {
    mlDemoInit = true;
    for (int gi = 0; gi < NUM_GROUPS; gi++)
      for (int i = 0; i < 32; i++)
        mlDemoFader[gi][i] = 0.75f;      // unity, as a console ships
  }
  mlGroup = 0; mlNum = 1;
  mlBuildPaths();
  mlDemoLoadStrip();
  mlDemoMs = millis();
}

static void mlDemoEnd() { mlDemo = false; mlMeterValid = false; }

// Plausible programme material so the meter and peak-hold behave realistically.
static void mlDemoTick() {
  if (!mlMetersActive || !elapsed(mlDemoMs, 60)) return;
  mlDemoMs = millis();
  const Group &g = GROUPS[mlGroup];
  if (g.meterBase < 0) { mlMeterValid = false; return; }
  float t = millis() / 1000.0f;
  float env = 0.45f + 0.35f * sinf(t * 1.7f) + 0.15f * sinf(t * 5.3f);
  if (random(100) < 6) env += 0.30f;                 // transients
  mlMeter = constrain(env * mlFader / 0.75f, 0.0f, 1.0f);
  mlMeterValid = true;
  mlLastRx = millis();
  if (mlMeter >= mlPeak) { mlPeak = mlMeter; mlPeakMs = millis(); }
}

static void mlSendFader(float f) {
  if (mlDemo) {
    const Group &g = GROUPS[mlGroup];
    mlDemoFader[mlGroup][constrain(g.numbered ? mlNum - 1 : 0, 0, 31)] = f;
    mlLastRx = millis();
    return;
  }
  oscFloat(mlFaderPath, f);
}

static bool mlLinkOk() { return mlDemo || (mlUdpUp && !elapsed(mlLastRx, 3000)); }

static void mlSetMeters(bool on) {
  mlMetersActive = on;
  if (on && mlUdpUp) { oscMeterSub(); mlLastMeter = millis(); }
}

static void mlHandleMeterBlob(const uint8_t *p, int len) {
  const Group &g = GROUPS[mlGroup];
  if (g.meterBase < 0 || len < 8) { mlMeterValid = false; return; }
  int32_t count = oscI32LE(p, 0);
  int idx = g.meterBase + (g.numbered ? (mlNum - 1) : 0);
  if (idx < 0 || idx >= count) { mlMeterValid = false; return; }

  float v;
  if (mxCfgModel == 1) {                    // X-Air: int16, dBFS * 256
    if (4 + (idx + 1) * 2 > len) return;
    float db = constrain(oscI16LE(p, 4 + idx * 2) / 256.0f, -90.0f, 0.0f);
    v = powf(10.0f, db / 20.0f);
  } else {                                  // X32/M32: float, linear 0..1
    if (4 + (idx + 1) * 4 > len) return;
    v = oscF32LE(p, 4 + idx * 4);
  }
  if (!(v >= 0.0f) || v > 16.0f) v = 0.0f;
  mlMeter = v; mlMeterValid = true;
  if (v >= mlPeak) { mlPeak = v; mlPeakMs = millis(); }
}

static void mlPollUdp() {
  while (udp.parsePacket() > 0) {
    int len = udp.read(mlRx, sizeof(mlRx) - 1);
    if (len <= 0) continue;
    mlRx[len] = 0;
    mlLastRx = millis();

    const char *addr = (const char *)mlRx;
    int off = strlen(addr); do { off++; } while (off & 3);
    if (off >= len) continue;
    const char *tags = (const char *)(mlRx + off);
    int toff = off + strlen(tags); do { toff++; } while (toff & 3);

    if (!strcmp(addr, mlFaderPath) && tags[0] == ',' && tags[1] == 'f') {
      if (toff + 4 <= len) { mlFader = oscF32BE(mlRx, toff); mlFaderSeq++; }
    } else if (!strcmp(addr, mlNamePath) && tags[0] == ',' && tags[1] == 's') {
      if (toff < len) {
        strncpy(mlName, (const char *)(mlRx + toff), sizeof(mlName) - 1);
        mlName[sizeof(mlName) - 1] = 0;
      }
    } else if (!strncmp(addr, "/meters/", 8) && tags[0] == ',' && tags[1] == 'b') {
      if (toff + 4 <= len) {
        int32_t nb = oscI32BE(mlRx, toff);
        if (toff + 4 + nb <= len) mlHandleMeterBlob(mlRx + toff + 4, nb);
      }
    }
  }
}

static void mlTick() {
  if (mlDemo) { mlDemoTick(); return; }
  if (!wifiOnline()) { mlUdpUp = false; return; }
  if (!mlUdpUp) {
    mlUdpUp = true;
    udp.begin((uint16_t)(mxCfgPort + 1));
    mlBuildPaths();
    oscXremote();
    mlRequestStrip();
    if (mlMetersActive) oscMeterSub();
    mlLastXremote = mlLastMeter = mlLastResync = millis();
  }
  mlPollUdp();
  if (elapsed(mlLastXremote, XREMOTE_MS)) { oscXremote(); mlLastXremote = millis(); }
  if (mlMetersActive && elapsed(mlLastMeter, METER_MS)) { oscMeterSub(); mlLastMeter = millis(); }
  if (elapsed(mlLastResync, RESYNC_MS)) { oscQuery(mlFaderPath); mlLastResync = millis(); }
}

// X32 fader taper: OSC float 0..1 is NOT dB. 0.75 == 0 dB, 1.0 == +10 dB.
static float faderToDb(float f) {
  if (f >= 0.5f)    return f * 40.0f  - 30.0f;
  if (f >= 0.25f)   return f * 80.0f  - 50.0f;
  if (f >= 0.0625f) return f * 160.0f - 70.0f;
  if (f >  0.0f)    return f * 480.0f - 90.0f;
  return -200.0f;
}

/* ###########################################################################
 * #                    ENCODER TEST + ENCODER CONFIG                        #
 * ######################################################################### */

static long etCount = 0;
static int  etLast = 0;
static uint32_t etLastMs = 0, etClicks = 0, etHolds = 0;

static void etEnter() { knobResetSteps(); etCount = 0; etClicks = etHolds = 0; }

static void etButton(BtnId b, BtnEv e) {
  if (e != EV_PRESS) return;
  if (b == B_ENC) etClicks++;
  else if (b == B_B) { etCount = 0; etClicks = etHolds = 0; }
}

static void etTick() {
  int s = knobSteps();
  if (s) { etCount += s; etLast = s; etLastMs = millis(); }
  if (btnHeldFor(B_ENC) > (uint32_t)encCfgHoldMs && !etHolds) etHolds = 1;
  if (!btnIsDown(B_ENC) && etHolds == 1) etHolds = 2;
}

static void etDraw() {
  drawTitle("Encoder Test");
  display.setTextSize(2);
  display.setCursor(0, 14);
  display.printf("%ld", etCount);
  display.setTextSize(1);

  display.setCursor(0, 32);
  display.printf("last %+d", etLast);
  display.setCursor(60, 32);
  display.printf("x%d", knobAccelMul());

  display.setCursor(0, 41);
  display.printf("clicks %lu", (unsigned long)etClicks);

  display.setCursor(0, 50);
  if (btnIsDown(B_ENC)) {
    uint32_t h = btnHeldFor(B_ENC);
    display.printf("HOLD %lums", (unsigned long)h);
  } else if (etHolds == 2) display.print("hold registered");
  else display.print("turn / click");

  display.setCursor(0, 58);
  display.print("B resets");
  drawButtonLabels(G_NONE, G_SELECT, G_NONE);
}

static const App AppEncoderTest = {
  "Encoder Test", nullptr, etEnter, nullptr, etTick, etDraw, etButton, nullptr,
  nullptr, nullptr
};

/* 1 means no multiplication, i.e. off. The range starts at 1 rather than 0
 * because with a 0..10 range both 0 and 1 rendered as "Off", so the first
 * press appeared to do nothing at all. */
static void fmtAccel(char *b, size_t n) {
  if (encCfgAccel <= 1) snprintf(b, n, "Off");
  else snprintf(b, n, "%ldx", (long)encCfgAccel);
}

static const CfgItem ENC_CFG_ITEMS[] = {
  ITEM_INT ("Per Detent", encCfgDiv,       1,   8, 1, ""),
  ITEM_BOOL("Reversed",   encCfgInvert),
  { "Accel",  C_INT, &encCfgAccel,     1,  10, 1, nullptr,0, nullptr,nullptr, fmtAccel, nullptr },
  ITEM_INT ("Accel Win",  encCfgAccelMs,  10, 200, 5, "ms"),
  ITEM_INT ("Hold",       encCfgHoldMs,  500,5000,100, "ms"),
};
static CfgPage PageEncCfg = { "Encoder Config", ENC_CFG_ITEMS, 5, 0, false, nullptr };
static void ecEnter() { cfgOpen(&PageEncCfg); }

static const App AppEncoderConfig = {
  "Configuration", nullptr, ecEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, nullptr
};

static const CfgItem ENC_ITEMS[] = {
  ITEM_LINK("Encoder Test",  AppEncoderTest),
  ITEM_LINK("Configuration", AppEncoderConfig),
};
static CfgPage PageEncoder = { "Encoder", ENC_ITEMS, 2, 0, false, nullptr };
static void emEnter() { cfgOpen(&PageEncoder); }

static const App AppEncoderMenu = {
  "Encoder", nullptr, emEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, nullptr
};

/* ###########################################################################
 * #                        MIXER SETTINGS                                   #
 * ######################################################################### */

#define ITEM_ACTF(l,fn,f) { l, C_ACT, nullptr,0,0,0, nullptr,0, nullptr, fn, f, nullptr }

static const char *const OPT_MODEL[] = { "M32R/X32", "X-Air" };

static void fmtMixerIp(char *b, size_t n) {
  if (!mixerIpSet()) snprintf(b, n, "Setup");
  else snprintf(b, n, "%s", mixerIp().toString().c_str());
}

static void mixerDemoStart();      // defined with the mixer app

static void mixerIpEdit() {
  ipInputBegin(&mxCfgIp, "Mixer IP", nullptr);
  gIpAllowDemo = true;
  gIpDemo = mixerDemoStart;
  navPush(&AppIpInput);
}

static void mixerIpClear() {
  mxCfgIp = 0;
  nvTouch(); nvFlush();
}

static const char *const OPT_MX_ENC[] = { "Hold+Turn Ch", "Hold = Rate B",
                                          "Click Group", "Off" };
static const char *const OPT_MX_ACC[] = { "Off", "Rate A", "Rate B", "Both" };

// Rates read as a percentage of full travel, which is what the bar shows.
static void fmtRateA(char *b, size_t n) {
  snprintf(b, n, "%ld.%ld%%", (long)(mxCfgRateA / 10), (long)(mxCfgRateA % 10));
}
static void fmtRateB(char *b, size_t n) {
  snprintf(b, n, "%ld.%ld%%", (long)(mxCfgRateB / 10), (long)(mxCfgRateB % 10));
}

static const CfgItem MIXER_CFG_ITEMS[] = {
  ITEM_ENUM("Model",     mxCfgModel, OPT_MODEL),
  ITEM_ACTF("IP",        mixerIpEdit, fmtMixerIp),
  ITEM_ACT ("Clear IP",  mixerIpClear),
  ITEM_ACT ("Demo Mode", mixerDemoStart),
  ITEM_INT ("OSC Port",  mxCfgPort,      1,65535, 1, ""),
  ITEM_ENUM("Enc Btn",   mxCfgEncBtn, OPT_MX_ENC),
  { "Rate A", C_INT, &mxCfgRateA, 1, 200, 1, nullptr,0, nullptr,nullptr, fmtRateA, nullptr },
  { "Rate B", C_INT, &mxCfgRateB, 1, 200, 1, nullptr,0, nullptr,nullptr, fmtRateB, nullptr },
  ITEM_ENUM("Accel On",  mxCfgAccelOn, OPT_MX_ACC),
  ITEM_INT ("Meter Smooth", mxCfgMeterSm, 0, 95, 5, "%"),
  ITEM_INT ("User Hold", mxCfgUserHold,100, 3000,50, "ms"),
  ITEM_INT ("Meter Rate",mxCfgMeterRate, 1,   20, 1, ""),
};
static CfgPage PageMixerCfg = { "Mixer", MIXER_CFG_ITEMS, 12, 0, false, nullptr };
static void mcEnter() { cfgOpen(&PageMixerCfg); }

static const App AppMixerSettings = {
  "Mixer", nullptr, mcEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, nullptr
};

/* ###########################################################################
 * #        SPEAKER LINK  --  Sony Audio Control API (ScalarWebAPI)          #
 * #                                                                         #
 * #  Verified against a Sony HT-NT5 (BAR-2016):                             #
 * #    POST http://<ip>:10000/sony/<service>   JSON, no auth                #
 * #    audio.getVolumeInformation 1.1 -> {volume,minVolume,maxVolume,step,  #
 * #                                       mute,output}                      #
 * #    audio.setAudioVolume       1.1 <- {output, volume}  (volume STRING)  #
 * #    avContent.getPlayingContentInfo 1.2 -> {uri, source, stateInfo{state}}#
 * #    avContent.pausePlayingContent   1.1 <- {output}                      #
 * #    avContent.setPlayNextContent    1.0 <- {output}                      #
 * #    avContent.setPlayPreviousContent 1.0 <- {output}                     #
 * #    avContent.setPlayContent        1.2 <- {uri, output}   (resume)      #
 * #  This unit reports output "" (single zone) and a 0..50 volume scale.    #
 * #  switchNotifications is websocket-only here, so state is polled.        #
 * ######################################################################### */

static int  spkVol = 0, spkVolMin = 0, spkVolMax = 50, spkStep = 1;
static bool spkMute = false, spkOnline = false;
// Read by netTask, written by the UI task: must not be cached in a register.
static volatile bool spkActive = false;
static char spkState[12] = "";          // PLAYING / PAUSED / STOPPED
static char spkUri[72] = "";
static uint32_t spkLastVolPoll = 0, spkLastStatePoll = 0, spkLastOk = 0;
static uint32_t spkQuietMs = 0, spkQuietLen = 0;
static uint32_t spkVolSeq = 0;
static uint32_t spkFailStreak = 0;   // consecutive unanswered polls   // bumped on every successful volume read
static char spkSource[40] = "";
static WiFiClient spkClient;      // kept alive so writes skip the TCP handshake

// Polling must stand down while the user is driving the knob, otherwise a
// stale reading gets treated as a remote change and drives the servo back.
static void spkQuiet(uint32_t ms) { spkQuietMs = millis(); spkQuietLen = ms; }

/* The two halves of this app are independent: volume is the Sony box on the
 * LAN, metadata and transport are Spotify's cloud. Either may be absent. */
static IPAddress spkIp() { return IPAddress((uint32_t)spCfgIp); }
static bool spkIpSet() { return spCfgIp != 0; }
/* The window has to outlast the poll interval, which now backs off when the
 * speaker is quiet. At 4s a backed-off poll cadence looked like a dead link
 * even while writes were succeeding. */
static bool spkLinkOk() {
  uint32_t win = 4000 + (uint32_t)spCfgPollMs * 4;
  return spkOnline && !elapsed(spkLastOk, win);
}

/* ---- tiny JSON scrapers (no allocator pressure, no library) ------------- */
static bool jsonInt(const String &s, const char *key, int *out) {
  String k = String("\"") + key + "\":";
  int i = s.indexOf(k);
  if (i < 0) return false;
  i += k.length();
  while (i < (int)s.length() && s[i] == ' ') i++;
  bool neg = false;
  if (i < (int)s.length() && s[i] == '-') { neg = true; i++; }
  if (i >= (int)s.length() || !isdigit((unsigned char)s[i])) return false;
  int v = 0;
  while (i < (int)s.length() && isdigit((unsigned char)s[i])) { v = v * 10 + (s[i] - '0'); i++; }
  *out = neg ? -v : v;
  return true;
}

/* Spotify pretty-prints: `"key" : value`, with spaces around the colon.
 * Matching on "key": therefore never fires. These scan a plain char buffer,
 * tolerate whitespace, and allocate nothing at all. */
static const char *cjFind(const char *from, const char *to, const char *key) {
  char k[40];
  snprintf(k, sizeof(k), "\"%s\"", key);
  size_t kl = strlen(k);
  const char *p = from;
  while (p && (p = strstr(p, k)) != nullptr) {
    if (to && p >= to) return nullptr;
    const char *q = p + kl;
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
    if (*q == ':') {
      q++;
      while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
      return q;
    }
    p = q;
  }
  return nullptr;
}

static const char *cjLast(const char *from, const char *before, const char *key) {
  char k[40];
  snprintf(k, sizeof(k), "\"%s\"", key);
  const char *best = nullptr, *p = from;
  while ((p = strstr(p, k)) != nullptr && p < before) { best = p; p += strlen(k); }
  return best;
}

static bool cjStr(const char *from, const char *to, const char *key,
                  char *out, size_t n) {
  const char *v = cjFind(from, to, key);
  if (!v || *v != '"') return false;
  v++;
  size_t i = 0;
  while (*v && *v != '"' && i < n - 1) {
    if (*v == '\\' && v[1]) v++;
    out[i++] = *v++;
  }
  out[i] = 0;
  return i > 0;
}

static bool cjNum(const char *from, const char *to, const char *key, int *out) {
  const char *v = cjFind(from, to, key);
  if (!v) return false;
  bool neg = (*v == '-');
  if (neg) v++;
  if (!isdigit((unsigned char)*v)) return false;
  long r = 0;
  while (isdigit((unsigned char)*v)) r = r * 10 + (*v++ - '0');
  *out = (int)(neg ? -r : r);
  return true;
}

/* Index-bounded variants. syParsePlayer used to do r.substring() on a ~3.4KB
 * response, which needs a second large allocation; when that failed the
 * parse silently found nothing and reported "no item". These copy directly
 * out of the original String instead. */
static bool jsonIntAt(const String &s, int from, int to, const char *key, int *out) {
  String k = String("\"") + key + "\":";
  int i = s.indexOf(k, from);
  if (i < 0 || (to >= 0 && i >= to)) return false;
  i += k.length();
  while (i < (int)s.length() && s[i] == ' ') i++;
  bool neg = false;
  if (i < (int)s.length() && s[i] == '-') { neg = true; i++; }
  if (i >= (int)s.length() || !isdigit((unsigned char)s[i])) return false;
  int v = 0;
  while (i < (int)s.length() && isdigit((unsigned char)s[i])) { v = v*10 + (s[i]-'0'); i++; }
  *out = neg ? -v : v;
  return true;
}

static bool jsonStrAt(const String &s, int from, int to, const char *key,
                      char *out, size_t n) {
  String k = String("\"") + key + "\":\"";
  int i = s.indexOf(k, from);
  if (i < 0 || (to >= 0 && i >= to)) return false;
  i += k.length();
  int e = s.indexOf('"', i);
  if (e < 0) return false;
  int len = min((int)n - 1, e - i);
  for (int j = 0; j < len; j++) out[j] = s[i + j];
  out[len] = 0;
  return true;
}

static bool jsonStr(const String &s, const char *key, char *out, size_t n) {
  String k = String("\"") + key + "\":\"";
  int i = s.indexOf(k);
  if (i < 0) return false;
  i += k.length();
  int e = s.indexOf('"', i);
  if (e < 0) return false;
  String v = s.substring(i, e);
  snprintf(out, n, "%s", v.c_str());
  return true;
}

static bool spkPost(const char *service, const String &body, String *resp) {
  if (!spkIpSet() || !wifiOnline()) return false;
  if (!netLock()) return false;
  HTTPClient http;
  http.setReuse(true);
  http.setConnectTimeout(600);
  if (!http.begin(spkClient, spkIp().toString(), (uint16_t)spCfgPort,
                  String("/sony/") + service, false))
    return false;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(900);
  int code = http.POST(body);
  bool ok = false;
  if (code == 200) {
    String r = http.getString();
    if (resp) *resp = r;
    ok = (r.indexOf("\"error\"") < 0);
  }
  http.end();
  if (ok) { spkLastOk = millis(); spkOnline = true; spkFailStreak = 0; }
  else {
    if (code <= 0) spkOnline = false;
    if (spkFailStreak < 16) spkFailStreak++;
  }
  netUnlock();
  return ok;
}

static String spkBody(const char *method, const char *params, const char *ver) {
  return String("{\"method\":\"") + method + "\",\"id\":9,\"params\":[" +
         params + "],\"version\":\"" + ver + "\"}";
}

static bool spkPollVolume() {
  String r;
  if (!spkPost("audio", spkBody("getVolumeInformation", "{\"output\":\"\"}", "1.1"), &r))
    return false;
  int v;
  if (jsonInt(r, "volume", &v))    spkVol = v;
  if (jsonInt(r, "minVolume", &v)) spkVolMin = v;
  if (jsonInt(r, "maxVolume", &v)) spkVolMax = v;
  if (jsonInt(r, "step", &v))      spkStep = max(1, v);
  char m[8];
  if (jsonStr(r, "mute", m, sizeof(m))) spkMute = !strcmp(m, "on");
  spkVolSeq++;
  return true;
}

static bool spkPollState() {
  String r;
  if (!spkPost("avContent", spkBody("getPlayingContentInfo", "{\"output\":\"\"}", "1.2"), &r))
    return false;
  jsonStr(r, "state", spkState, sizeof(spkState));
  jsonStr(r, "uri", spkUri, sizeof(spkUri));
  jsonStr(r, "source", spkSource, sizeof(spkSource));
  return true;
}

/* Diagnostic: ask the soundbar which playback functions it believes are
 * available right now. If this comes back empty while Spotify Connect is
 * streaming, avContent does not model the session and transport control
 * over the local API is impossible for this source.                        */
static bool spkPollFunctions(String *raw) {
  return spkPost("avContent",
                 spkBody("getAvailablePlaybackFunction", "{\"output\":\"\"}", "1.0"),
                 raw);
}

// setAudioVolume takes the level as a STRING, not an int.
static bool spkSetVolume(int v) {
  v = constrain(v, spkVolMin, spkVolMax);
  String pr = String("{\"output\":\"\",\"volume\":\"") + v + "\"}";
  return spkPost("audio", spkBody("setAudioVolume", pr.c_str(), "1.1"), nullptr);
}

static bool spkPlaying() { return !strcmp(spkState, "PLAYING"); }

static bool spkSetMute(bool on) {
  String pr = String("{\"output\":\"\",\"mute\":\"") + (on ? "on" : "off") + "\"}";
  bool ok = spkPost("audio", spkBody("setAudioMute", pr.c_str(), "1.1"), nullptr);
  if (ok) spkMute = on;
  return ok;
}

static void spkNext() { spkPost("avContent", spkBody("setPlayNextContent", "{\"output\":\"\"}", "1.0"), nullptr); spkLastStatePoll = 0; }
static void spkPrev() { spkPost("avContent", spkBody("setPlayPreviousContent", "{\"output\":\"\"}", "1.0"), nullptr); spkLastStatePoll = 0; }

static void spkPlayPause() {
  if (spkPlaying()) {
    spkPost("avContent", spkBody("pausePlayingContent", "{\"output\":\"\"}", "1.1"), nullptr);
  } else {
    // No "resume" verb exists; re-issuing the current URI restarts the source.
    const char *uri = spkUri[0] ? spkUri : "netService:audio?service=spotify";
    String pr = String("{\"uri\":\"") + uri + "\",\"output\":\"\"}";
    spkPost("avContent", spkBody("setPlayContent", pr.c_str(), "1.2"), nullptr);
  }
  spkLastStatePoll = 0;      // refresh the icon promptly
}

static void spkService() {
  if (!spkActive || !spkIpSet() || !wifiOnline()) return;
  if (!elapsed(spkQuietMs, spkQuietLen)) return;
  /* An unreachable speaker costs a full connect timeout per attempt. Polling
   * it every 500ms regardless meant netTask spent nearly all its time failing
   * to reach a box that is not there, which starved the Spotify poll behind
   * it -- metadata never arrived while transport, being queued on demand,
   * still worked. Back off hard once it stops answering. */
  uint32_t mul = 1u << (spkFailStreak > 4 ? 4 : spkFailStreak);
  if (elapsed(spkLastVolPoll, (uint32_t)spCfgPollMs * mul)) {
    spkLastVolPoll = millis();
    netPollVol = true;
  } else if (elapsed(spkLastStatePoll, (uint32_t)spCfgStateMs * mul)) {
    spkLastStatePoll = millis();
    netPollState = true;
  }
}

// The knob spans [spCfgVolMin..spCfgVolMax], clamped to what the device
// actually supports. Values outside the window are still displayed correctly;
// the window only decides what the knob's travel maps onto.
static int spkUserMin() { return constrain(spCfgVolMin, spkVolMin, spkVolMax); }
static int spkUserMax() { return constrain(spCfgVolMax, spkUserMin() + 1, spkVolMax); }

static float spkVolToFrac(int v) {
  int span = spkUserMax() - spkUserMin();
  if (span <= 0) return 0.0f;
  return constrain((float)(v - spkUserMin()) / span, 0.0f, 1.0f);
}
static int spkFracToVol(float f) {
  int span = spkUserMax() - spkUserMin();
  return spkUserMin() + (int)lroundf(constrain(f, 0.0f, 1.0f) * span);
}

/* ###########################################################################
 * #                    SPEAKER DISCOVERY (SSDP)                             #
 * ######################################################################### */

struct SonyDev { char ip[16]; char name[26]; char loc[86]; bool named; };
static SonyDev spcDev[8];
static int spcN = 0, spcSel = 0, spcResolve = 0, spcPhase = 0;  // 0 search 1 resolve 2 done
static uint32_t spcStart = 0;
static WiFiUDP spcUdp;

static void spcSendSearch() {
  String m = String("M-SEARCH * HTTP/1.1\r\n") +
             "HOST: 239.255.255.250:1900\r\n" +
             "MAN: \"ssdp:discover\"\r\n" +
             "MX: 3\r\n" +
             "ST: urn:schemas-sony-com:service:ScalarWebAPI:1\r\n\r\n";
  for (int i = 0; i < 3; i++) {
    spcUdp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
    spcUdp.print(m);
    spcUdp.endPacket();
    delay(80);
  }
}

static void spcEnter() {
  knobInputMode();
  spcN = spcSel = spcResolve = 0;
  spcPhase = 0;
  spcUdp.begin(0);
  spcStart = millis();
  spcSendSearch();
}

static void spcExit() { spcUdp.stop(); }

static void spcCollect() {
  char buf[760];
  int n = spcUdp.parsePacket();
  if (n <= 0) return;
  int len = spcUdp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = 0;

  String s(buf);
  int li = s.indexOf("LOCATION:");
  if (li < 0) li = s.indexOf("Location:");
  if (li < 0) return;
  int e = s.indexOf('\r', li);
  String loc = s.substring(li + 9, e);
  loc.trim();

  int hs = loc.indexOf("://");
  if (hs < 0) return;
  int he = loc.indexOf(':', hs + 3), hp = loc.indexOf('/', hs + 3);
  int endHost = (he > 0 && (hp < 0 || he < hp)) ? he : hp;
  if (endHost < 0) return;
  String ip = loc.substring(hs + 3, endHost);

  for (int i = 0; i < spcN; i++) if (ip == spcDev[i].ip) return;   // dedupe
  if (spcN >= 8) return;
  snprintf(spcDev[spcN].ip, sizeof(spcDev[spcN].ip), "%s", ip.c_str());
  snprintf(spcDev[spcN].name, sizeof(spcDev[spcN].name), "%s", ip.c_str());
  snprintf(spcDev[spcN].loc, sizeof(spcDev[spcN].loc), "%s", loc.c_str());
  spcDev[spcN].named = false;
  spcN++;
}

// The description XML carries friendlyName; fetched one device per tick so the
// UI keeps redrawing instead of freezing for the whole resolve pass.
static void spcResolveOne() {
  SonyDev &d = spcDev[spcResolve];
  HTTPClient http;
  if (http.begin(d.loc)) {
    http.setTimeout(2500);
    if (http.GET() == 200) {
      String xml = http.getString();
      int a = xml.indexOf("<friendlyName>");
      if (a >= 0) {
        int b = xml.indexOf("</friendlyName>", a);
        if (b > a) snprintf(d.name, sizeof(d.name), "%s", xml.substring(a + 14, b).c_str());
      }
    }
    http.end();
  }
  d.named = true;
  spcResolve++;
}

static void spcTick() {
  if (spcPhase == 0) {
    spcCollect();
    if (elapsed(spcStart, 4500)) { spcUdp.stop(); spcPhase = spcN ? 1 : 2; }
  } else if (spcPhase == 1) {
    if (spcResolve >= spcN) spcPhase = 2;
    else spcResolveOne();
  }
}

static void spkAfterConfigured() {
  nvTouch();
  nvFlush();
  if (gSpkGateway && gAfterConnect) {
    const App *t = gAfterConnect;
    gAfterConnect = nullptr;
    gSpkGateway = false;
    navHome();
    launchApp(t);
  } else navBack();
}

static void spcSelect() {
  if (spcPhase != 2 || !spcN) return;
  IPAddress a;
  if (!a.fromString(spcDev[spcSel].ip)) return;
  spCfgIp = (int32_t)(uint32_t)a;
  snprintf(spName, sizeof(spName), "%s", spcDev[spcSel].name);
  spkAfterConfigured();
}

static void spcDraw() {
  if (spcPhase != 2 || !spcN) uiWide();
  drawTitle("Scan");
  if (spcPhase == 0) {
    display.setCursor(0, 28);
    display.print("Searching...");
    drawBar(0, 44, CONTENT_W, 7, (float)(millis() - spcStart) / 4500.0f);
    return;
  }
  if (spcPhase == 1) {
    display.setCursor(0, 28);
    display.printf("Reading %d/%d", spcResolve + 1, spcN);
    return;
  }
  if (!spcN) {
    display.setCursor(0, 26);
    display.print("No Sony devices");
    display.setCursor(0, 38);
    display.print("found. Use");
    display.setCursor(0, 48);
    display.print("Manual IP.");
    return;
  }
  int first = (spcSel >= ROWS_VIS) ? spcSel - ROWS_VIS + 1 : 0;
  for (int i = 0; i < ROWS_VIS && first + i < spcN; i++) {
    int idx = first + i;
    drawRow(ROW_TOP + i * ROW_H, idx == spcSel, false,
            spcDev[idx].name, nullptr, G_RIGHT);
  }
  drawScrollbar(spcN, first);
  drawButtonLabels(G_UP, G_SELECT, G_DOWN);
}

static void spcMove(int d) {
  if (spcPhase != 2 || !spcN) return;
  spcSel = constrain(spcSel + d, 0, spcN - 1);
}
static void spcButton(BtnId b, BtnEv e) {
  if (b == B_C) spcMove(-1);
  else if (b == B_A) spcMove(+1);
  else if (btnSelect(b) && e == EV_PRESS) spcSelect();
}
static void spcKnob(int st) { if (st) spcMove(st > 0 ? +1 : -1); }

static const App AppSonyScan = {
  "Scan", nullptr, spcEnter, spcExit, spcTick, spcDraw, spcButton, spcKnob,
  nullptr, nullptr
};

/* ###########################################################################
 * #                       SPEAKER INFO PAGE                                 #
 * ######################################################################### */

/* The info page is a scrollable text buffer rather than a fixed layout, so
 * long URIs and raw API responses can actually be read on a 128x64 panel.  */
/* Speaker Info is declared before the Spotify module but reports on it, so
 * the handful of values it displays are declared up front. */
extern char  syTitle[110];
extern char  syErr[72];
extern int   syTlsErr;
extern char  syTlsErrTxt[64];
extern char *syBody;
extern const size_t SY_BODY_CAP;
extern int   syBodyLen;
extern bool  syHasTrack;
extern bool  syPolled;
extern int   syLastCode;
extern uint32_t syPollCount, syFailStreak;
extern volatile bool syActive;
extern bool  syTlsLive;
static bool syConfigured();
extern char syRefresh[220];

static const int SPI_MAX = 44, SPI_W = 17, SPI_VIS = 5, SPI_TOP = 14, SPI_LH = 10;
static char spiLine[SPI_MAX][SPI_W + 1];
static int  spiN = 0, spiTop = 0;

static void spiPush(const char *fmt, ...) {
  if (spiN >= SPI_MAX) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(spiLine[spiN], SPI_W + 1, fmt, ap);
  va_end(ap);
  spiN++;
}

static void spiWrap(const char *prefix, const char *text) {
  char buf[220];
  snprintf(buf, sizeof(buf), "%s%s", prefix, text);
  int len = strlen(buf);
  if (!len) return;
  for (int i = 0; i < len && spiN < SPI_MAX; i += SPI_W)
    spiPush("%.*s", SPI_W, buf + i);
}

static void spiEnter() {
  knobInputMode();
  spkActive = true;
  spkQuiet(0);
  spkLastVolPoll = spkLastStatePoll = 0;
  spiN = spiTop = 0;

  bool haveSpk = spkIpSet();
  if (!haveSpk) { spiPush("No speaker set."); spiPush(""); }

  if (haveSpk) {
  spkPollVolume();
  String rawPlay, rawFn;
  spkPost("avContent", spkBody("getPlayingContentInfo", "{\"output\":\"\"}", "1.2"), &rawPlay);
  jsonStr(rawPlay, "state", spkState, sizeof(spkState));
  jsonStr(rawPlay, "uri", spkUri, sizeof(spkUri));
  jsonStr(rawPlay, "source", spkSource, sizeof(spkSource));
  bool fnOk = spkPollFunctions(&rawFn);

  Serial.println("---- getPlayingContentInfo ----");
  Serial.println(rawPlay);
  Serial.println("---- getAvailablePlaybackFunction ----");
  Serial.println(rawFn);

  spiWrap("NAME ", spName[0] ? spName : "(unknown)");
  spiPush("IP   %s", spkIp().toString().c_str());
  spiPush("VOL  %d (%d-%d)", spkVol, spkVolMin, spkVolMax);
  spiPush("KNOB %d-%d", spkUserMin(), spkUserMax());
  spiPush("MUTE %s", spkMute ? "on" : "off");
  spiPush("LINK %s", spkLinkOk() ? "ok" : "no response");
  spiPush("");
  spiPush("STATE %s", spkState[0] ? spkState : "(none)");
  spiWrap("SRC  ", spkSource[0] ? spkSource : "(none)");
  spiWrap("URI  ", spkUri[0] ? spkUri : "(none)");
  spiPush("");
  spiPush("-- playback fn --");
  if (!fnOk) spiPush("query failed");
  else spiWrap("", rawFn.c_str());
  spiPush("");
  }

  /* Spotify side. Shown here because the S2 talks over native USB CDC and
   * serial output is unavailable unless USB CDC On Boot is enabled, so the
   * device has to be able to explain itself on its own screen. */
  spiPush("-- spotify --");
  spiPush("cfg %s auth %s", syConfigured() ? "y" : "n", syRefresh[0] ? "y" : "n");
  spiPush("wifi %s act %s", wifiOnline() ? "y" : "n", syActive ? "y" : "n");
  spiPush("polls %lu", (unsigned long)syPollCount);
  spiPush("last code %d", syLastCode);
  spiPush("body %d B", syBodyLen);
  spiPush("track %s", syHasTrack ? "yes" : "no");
  spiPush("fails %lu", (unsigned long)syFailStreak);
  spiPush("psram %luk ext %s", (unsigned long)(ESP.getPsramSize() / 1024),
          gExtMalloc ? "yes" : "NO");
  /* The line that matters for a handshake. mbedTLS is pinned to internal RAM
   * by the core's sdkconfig unless it is redirected at runtime, so "tlspool"
   * says which heap the 2x16.5KB record buffers are actually coming out of,
   * and "psblk" is the headroom in that heap. */
  spiPush("tlspool %s", gTlsPsram ? "psram" : "internal");
  spiPush("psblk %luk", (unsigned long)(ESP.getMaxAllocPsram() / 1024));
  spiPush("rssi %d dBm", (int)WiFi.RSSI());
  spiPush("maxblk %luk", (unsigned long)(ESP.getMaxAllocHeap() / 1024));
  spiPush("heap %luk tls %s", (unsigned long)(ESP.getFreeHeap() / 1024),
          syTlsLive ? "up" : "down");
  if (syTlsErr) spiPush("tlserr -0x%04X", (unsigned)(-syTlsErr));
  if (syTlsErrTxt[0]) spiWrap("", syTlsErrTxt);
  if (syErr[0]) spiWrap("err ", syErr);
  if (syTitle[0]) spiWrap("t: ", syTitle);
}

static void spiExit() { spkActive = false; }

static void spiMove(int d) {
  int maxTop = max(0, spiN - SPI_VIS);
  spiTop = constrain(spiTop + d, 0, maxTop);
}

static void spiDraw() {
  drawTitle("Speaker");
  for (int i = 0; i < SPI_VIS && spiTop + i < spiN; i++)
    printClipped(0, SPI_TOP + i * SPI_LH, spiLine[spiTop + i], CONTENT_W - 5);

  if (spiN > SPI_VIS) {
    int h = SPI_VIS * SPI_LH;
    display.drawRect(SCROLL_X, SPI_TOP, 3, h, SH110X_WHITE);
    int th = max(4, h * SPI_VIS / spiN);
    int ty = SPI_TOP + (h - th) * spiTop / (spiN - SPI_VIS);
    display.fillRect(SCROLL_X, ty, 3, th, SH110X_WHITE);
  }
  drawButtonLabels(G_UP, G_NONE, G_DOWN);
}

static void spiButton(BtnId b, BtnEv e) {
  (void)e;
  if (b == B_C) spiMove(-1);
  else if (b == B_A) spiMove(+1);
}
static void spiKnob(int st) { if (st) spiMove(st > 0 ? +1 : -1); }

static const App AppSpeakerInfo = {
  "Speaker info", nullptr, spiEnter, spiExit, nullptr, spiDraw, spiButton, spiKnob,
  nullptr, nullptr
};

/* ###########################################################################
 * #                      SPOTIFY WEB API                                    #
 * #                                                                         #
 * #  The soundbar never exposes the Spotify Connect stream to its local     #
 * #  avContent service, so metadata and transport have to come from         #
 * #  Spotify's cloud. Volume stays on the Sony local API, which is why the  #
 * #  knob remains instant while skip/pause take a cloud round trip.         #
 * #                                                                         #
 * #  Auth: the device holds a refresh token and mints access tokens itself. #
 * #  Spotify only permits HTTPS or loopback redirect URIs, so the device    #
 * #  cannot host the OAuth callback; the code is pasted in via the built-in #
 * #  config page instead. See SPOTIFY_SETUP.md.                             #
 * ######################################################################### */

static const char *SY_REDIRECT = "http://127.0.0.1:8888/callback";
static const char *SY_SCOPE_ENC =
    "user-read-playback-state%20user-modify-playback-state";

static WiFiClientSecure syTls;
/* A kept-alive TLS session is fast but expensive. Hold it while the user is
 * active, then drop it so the memory comes back. */
bool     syTlsLive = false;
static uint32_t syTlsUsed = 0;

static void syTlsIdle(uint32_t ms) {
  if (!syTlsLive || !elapsed(syTlsUsed, ms)) return;
  syTls.stop();
  syTlsLive = false;
}
static char  syAccess[420] = "";
static uint32_t syTokenGot = 0, syTokenTtl = 0;
char  syTitle[110] = "";
static char syArtist[80] = "";
static bool  syIsPlaying = false;
bool  syHasTrack = false;
static int   syProgressMs = 0, syDurationMs = 0;
static uint32_t syFetchMs = 0, syLastPoll = 0;
char  syErr[72] = "";
bool  syPolled = false;   // has any poll actually completed?
int   syLastCode = 0;     // last HTTP status, for on-screen diagnosis
uint32_t syPollCount = 0;
/* Response buffer, placed in PSRAM when the board has it. On the FeatherS2
 * that hands 4.6KB of internal SRAM back to the heap, which is the pool a TLS
 * handshake has to allocate its record buffers from. */
const size_t SY_BODY_CAP = 4608;
char *syBody = nullptr;
int   syBodyLen = 0;
static char  syRaw[160] = "";      // last token-endpoint body, for diagnosis
/* mbedTLS's own verdict on the last failed handshake. HTTPClient flattens
 * every possible cause into -1, which is what made six versions of this bug
 * unfalsifiable: a refused socket, a timeout and an out-of-memory handshake
 * all printed the same number. NetworkClientSecure keeps the real code across
 * the stop() that follows a failed connect, so it can simply be read back. */
int   syTlsErr = 0;
char  syTlsErrTxt[64] = "";
volatile bool syActive = false;

bool syConfigured() { return syClientId[0] && sySecret[0]; }
static bool syAuthorized() { return syConfigured() && syRefresh[0]; }

// Minimal base64 and percent-encoding so the token request needs no extra
// library and cannot be tripped up by odd characters in a pasted code.
static String syB64(const String &in) {
  static const char *T =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String o;
  int i = 0, len = in.length();
  while (i < len) {
    uint32_t v = 0;
    int nb = 0;
    for (int k = 0; k < 3; k++) {
      v <<= 8;
      if (i < len) { v |= (uint8_t)in[i++]; nb++; }
    }
    o += T[(v >> 18) & 63];
    o += T[(v >> 12) & 63];
    o += (nb > 1) ? T[(v >> 6) & 63] : '=';
    o += (nb > 2) ? T[v & 63] : '=';
  }
  return o;
}

static String syUrlEnc(const String &v) {
  String o;
  char b[4];
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~')
      o += c;
    else { snprintf(b, sizeof(b), "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

/* Accepts the bare code, or the whole redirect URL, or anything with trailing
 * query junk such as "?ubi=..." which Spotify sometimes appends.            */
static String syCleanCode(String c) {
  c.trim();
  int i = c.indexOf("code=");
  if (i >= 0) c = c.substring(i + 5);
  int cut = c.length();
  const char *delims = "&?# \t";
  for (const char *d = delims; *d; d++) {
    int j = c.indexOf(*d);
    if (j >= 0 && j < cut) cut = j;
  }
  c = c.substring(0, cut);
  c.trim();
  return c;
}

static void syTlsInit() {
  static bool done = false;
  if (done) return;
  // No cert pinning: Spotify rotates roots and a stale pin would brick the
  // device. Traffic is still encrypted; only server identity is unverified.
  syTls.setInsecure();
  done = true;
}

static int syProgressNow() {
  if (!syHasTrack) return 0;
  int p = syProgressMs;
  if (syIsPlaying) p += (int)(millis() - syFetchMs);
  return constrain(p, 0, syDurationMs > 0 ? syDurationMs : p);
}

/* Read mbedTLS's error for the connection that just failed. -0x7F00
 * (SSL_ALLOC_FAILED) means the record buffers would not fit; -0x7280 is a
 * fatal alert from the far end; 0 with a -1 from HTTPClient means the TCP
 * socket never opened, so it never reached TLS at all. */
/* Cleared on any request that reached the far end, so the info page cannot
 * show a stale failure next to a working link -- the same trap syErr fell
 * into from the other direction in v9.4. */
static void syNoteTls(int code) {
  if (code > 0) { syTlsErr = 0; syTlsErrTxt[0] = 0; return; }
  char buf[sizeof(syTlsErrTxt)] = "";
  syTlsErr = syTls.lastError(buf, sizeof(buf));
  snprintf(syTlsErrTxt, sizeof(syTlsErrTxt), "%s", buf);
}

/* ---- token handling ----------------------------------------------------- */
static bool syTokenRequest(const String &body) {
  if (!wifiOnline()) return false;
  /* The speaker is reached by IP and Spotify by name, so resolution is the
   * one step only Spotify depends on. Resolve explicitly first, so a name
   * failure reports as DNS instead of folding into the same -1 that a
   * refused or timed-out connection gives. */
  IPAddress sip;
  if (!WiFi.hostByName("accounts.spotify.com", sip)) {
    snprintf(syErr, sizeof(syErr), "DNS fail rssi %d", (int)WiFi.RSSI());
    return false;
  }
  /* getMaxAllocHeap() is the largest free INTERNAL block, and with the
   * mbedTLS allocator redirected to PSRAM that is no longer the pool the
   * handshake draws its 2x16.5KB record buffers from. The guard therefore
   * only applies on a board without PSRAM, where it needs the full ~33KB
   * plus room for the certificate chain in one piece. */
  uint32_t blk = ESP.getMaxAllocHeap();
  if (!gTlsPsram && blk < 36000) {
    // Report the real reason rather than letting it look like a DNS fault.
    snprintf(syErr, sizeof(syErr), "heap blk %u", (unsigned)blk);
    return false;
  }
  if (!syLock()) return false;
  syTlsInit();
  syTls.stop();                       // accounts host differs from api host
  syTlsLive = false;                  // the API session is gone with it
  HTTPClient http;
  /* An ECDHE handshake plus certificate work takes well over a second on
   * this chip, and DNS can add another. 2.5s was simply too short a window
   * for the connection to complete, and a timed-out connect surfaces as the
   * same -1 as a genuine DNS failure. It all happens in netTask, so a longer
   * wait costs the UI nothing. */
  /* Back down from 15s. netTask is a single task: while it sits in a long
   * Spotify connect it cannot service a queued volume write, so 15s plus an
   * automatic retry could monopolise it for most of a minute. That is what
   * stopped the speaker responding again -- my own change, not a new fault. */
  // syTask is independent now, so a long wait no longer stalls anything.
  http.setConnectTimeout(12000);
  http.setTimeout(12000);
  if (!http.begin(syTls, "accounts.spotify.com", 443, "/api/token", true)) {
    snprintf(syErr, sizeof(syErr), "tls begin fail");
    syUnlock();
    return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  // Spotify documents HTTP Basic for this endpoint; body-only credentials are
  // rejected by some accounts.
  http.addHeader("Authorization",
                 String("Basic ") + syB64(String(syClientId) + ":" + sySecret));
  int code = http.POST(body);
  if (code > 0) syNoteTls(code);      // reached the far end: no TLS fault
  bool ok = false;
  String r = (code > 0) ? http.getString() : String("");
  snprintf(syRaw, sizeof(syRaw), "%s", r.c_str());
  Serial.printf("[token] HTTP %d\n%s\n", code, r.c_str());
  if (code == 200) {
    if (jsonStr(r, "access_token", syAccess, sizeof(syAccess))) {
      int ttl = 3600;
      jsonInt(r, "expires_in", &ttl);
      syTokenGot = millis();
      syTokenTtl = (uint32_t)ttl * 1000;
      // A good token says nothing about the poll that follows, so it must not
      // clear a poll's error either.
      char nr[220];
      if (jsonStr(r, "refresh_token", nr, sizeof(nr)) && nr[0]) {
        snprintf(syRefresh, sizeof(syRefresh), "%s", nr);
        nvTouch();
        nvFlush();
      }
      ok = true;
    } else snprintf(syErr, sizeof(syErr), "no access_token in reply");
  } else if (code <= 0) {
    /* -1 from HTTPClient only says "did not connect". mbedTLS knows which of
     * the several possible reasons it was, so ask it rather than inferring
     * from the heap figure -- inference is what kept sending this in
     * circles. */
    syNoteTls(code);
    if (syTlsErr)
      snprintf(syErr, sizeof(syErr), "tls -0x%04X blk %uk", (unsigned)(-syTlsErr),
               (unsigned)(ESP.getMaxAllocHeap() / 1024));
    else
      snprintf(syErr, sizeof(syErr), "no socket blk %uk",
               (unsigned)(ESP.getMaxAllocHeap() / 1024));
  } else {
    char desc[90] = "";
    if (!jsonStr(r, "error_description", desc, sizeof(desc)))
      jsonStr(r, "error", desc, sizeof(desc));
    snprintf(syErr, sizeof(syErr), "HTTP %d %s", code, desc);
  }
  http.end();
  syTls.stop();
  syTlsLive = false;
  syUnlock();
  return ok;
}

static bool syExchangeCode(const String &rawCode) {
  String code = syUrlEnc(syCleanCode(rawCode));
  Serial.printf("[token] exchanging code len=%d\n", code.length());
  String b = "grant_type=authorization_code&code=" + code +
             "&redirect_uri=http%3A%2F%2F127.0.0.1%3A8888%2Fcallback" +
             "&client_id=" + syClientId;
  return syTokenRequest(b);
}

static bool syRefreshAccess() {
  if (!syAuthorized()) return false;
  String b = String("grant_type=refresh_token&refresh_token=") +
             syUrlEnc(syRefresh) + "&client_id=" + syClientId;
  return syTokenRequest(b);
}

static bool syEnsureToken() {
  if (!syAuthorized()) return false;
  if (syAccess[0] && !elapsed(syTokenGot, syTokenTtl > 60000 ? syTokenTtl - 60000 : 30000))
    return true;
  return syRefreshAccess();
}

/* ---- API calls ---------------------------------------------------------- */
static int syRequestOnce(const char *method, const char *path, bool wantBody) {
  if (!syLock()) return -1;
  syTlsInit();
  HTTPClient http;
  http.setReuse(true);
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  if (!http.begin(syTls, "api.spotify.com", 443, path, true)) { syUnlock(); return -1; }
  http.addHeader("Authorization", String("Bearer ") + syAccess);
  if (strcmp(method, "GET")) http.addHeader("Content-Length", "0");
  int code = http.sendRequest(method, (uint8_t *)nullptr, 0);
  int declared = http.getSize();
  /* Only claim a live session when there actually is one. Setting this
   * unconditionally left syTlsLive true after a failed connect, which then
   * told syRequest's heap guard that no handshake was pending and let a
   * request through on a third of the memory it really needed. */
  syTlsLive = (code > 0);
  syTlsUsed = millis();
  syNoteTls(code);

  syBodyLen = 0;
  if (code == 200 && wantBody && syBody) {
    // Stream straight into a static buffer. getString() had to grow a heap
    // String to ~4KB while TLS was resident, which is what drove free heap
    // down to ~13KB and made the device fall over intermittently.
    WiFiClient *st = http.getStreamPtr();
    uint32_t t0 = millis();
    while (st && syBodyLen < (int)SY_BODY_CAP - 1) {
      if (declared >= 0 && syBodyLen >= declared) break;
      int avail = st->available();
      if (avail > 0) {
        int room = (int)SY_BODY_CAP - 1 - syBodyLen;
        int rd = st->readBytes(syBody + syBodyLen, avail < room ? avail : room);
        if (rd <= 0) break;
        syBodyLen += rd;
        t0 = millis();
      } else {
        if (!st->connected()) break;
        if (elapsed(t0, 2500)) break;
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    }
  }
  if (code >= 400 && code < 500) {
    // Spotify puts the actual reason in the error body; keep it for the UI.
    String e = http.getString();
    snprintf(syBody, SY_BODY_CAP, "%s", e.c_str());
    syBodyLen = (int)strlen(syBody);
  }
  syBody[syBodyLen < 0 ? 0 : syBodyLen] = 0;
  http.end();
  syUnlock();
  Serial.printf("[sy] %s -> %d  len=%d declared=%d heap=%u\n", path, code,
                syBodyLen, declared, (unsigned)ESP.getFreeHeap());
  return code;
}

/* Wraps the request so the UI can tell "working on it" from "finished and
 * found nothing". Without this the display showed the idle message during
 * every request, because syErr is cleared on entry and the answer has not
 * arrived yet -- which read as a wrong answer rather than a pending one. */
static int syRequest(const char *method, const char *path, bool wantBody);
static int syRequestTracked(const char *method, const char *path, bool wantBody) {
  gSyBusy = true;
  int c = syRequest(method, path, wantBody);
  gSyBusy = false;
  return c;
}

static int syRequest(const char *method, const char *path, bool wantBody) {
  if (!wifiOnline()) { snprintf(syErr, sizeof(syErr), "wifi down"); return -1; }
  /* syErr is NOT cleared here any more. Clearing on entry meant the last real
   * failure was erased at the start of the next attempt, so between a failed
   * poll and the next one the screen fell through to the idle text -- which
   * is why "Spotify idle" was showing far more often than the actual error.
   * Only a poll that genuinely succeeds clears it. */
  /* The old floor was 45KB, but a live TLS session holds ~33KB of record
   * buffers open between requests (setReuse), so free heap sits near 12KB
   * once the first request has run. That guard therefore blocked every
   * metadata poll while transport commands slipped through in the moments
   * heap happened to be recovered -- exactly the observed behaviour. The
   * response body is a static buffer, so a poll needs very little heap; a
   * handshake needs far more, and syTlsIdle() below frees it when idle. */
  uint32_t h = ESP.getFreeHeap();
  /* 45000 was measured before the NeoPixel driver and battery code existed.
   * Those pushed the idle heap down just far enough that a fresh handshake
   * never passed, so Spotify stopped entirely while the speaker -- plain
   * HTTP, no TLS, no guard -- carried on working. A handshake needs roughly
   * 30KB; the floor now reflects that with a little margin rather than a
   * figure chosen when there was room to spare. */
  /* With mbedTLS allocating from PSRAM the internal heap only has to cover
   * the socket and the small odds and ends, so the old 32KB floor -- which
   * described the record buffers -- no longer describes anything real. It
   * still applies on a board with no PSRAM. */
  uint32_t need = gTlsPsram ? 12000 : (syTlsLive ? 12000 : 36000);
  if (h < need) {
    snprintf(syErr, sizeof(syErr), "low heap %u", (unsigned)h);
    return -1;
  }
  if (!syEnsureToken()) return -1;
  /* A kept-alive session the far end has already closed fails on send, not on
   * connect, and looks identical to "no network". Drop it and try once more
   * with a fresh handshake before believing the failure. Whether the session
   * was being reused has to be sampled BEFORE the call: syRequestOnce now
   * clears syTlsLive on failure, so testing it afterwards would never see a
   * reuse and the retry would never fire. */
  bool reused = syTlsLive;
  int code = syRequestOnce(method, path, wantBody);
  if (code <= 0 && reused) {
    // Retry only a reused session the far end may have closed. Retrying a
    // fresh failure just doubles how long syTask is unavailable.
    syTls.stop();
    syTlsLive = false;
    code = syRequestOnce(method, path, wantBody);
  }
  if (code == 401) {
    syAccess[0] = 0;
    if (syEnsureToken()) code = syRequestOnce(method, path, wantBody);
  }
  return code;
}

/* Spotify orders object keys alphabetically, so within `item` the sequence is
 * album, artists, ..., duration_ms, ..., name. Anchoring on duration_ms gives
 * the track name unambiguously, and the last `artists` before it is the
 * track's own artist list rather than the album's.                          */
static void syParsePlayer(const char *r) {
  syTitle[0] = syArtist[0] = 0;

  int n;
  if (!syLocalActive()) {
    const char *ip = cjFind(r, nullptr, "is_playing");
    syIsPlaying = ip && !strncmp(ip, "true", 4);
    syProgressMs = cjNum(r, nullptr, "progress_ms", &n) ? n : 0;
    syFetchMs = millis();
  }

  const char *dv = cjFind(r, nullptr, "duration_ms");
  if (!dv) { syHasTrack = false; syDurationMs = 0; return; }
  if (cjNum(r, nullptr, "duration_ms", &n)) syDurationMs = n;

  // Within `item` the keys run album, artists, ..., duration_ms, ..., name,
  // so anchoring on duration_ms yields the track rather than the album.
  cjStr(dv, nullptr, "name", syTitle, sizeof(syTitle));
  const char *ar = cjLast(r, dv, "artists");
  if (ar) cjStr(ar, dv, "name", syArtist, sizeof(syArtist));

  syHasTrack = syTitle[0] != 0;
}

uint32_t syFailStreak = 0;
static uint32_t sySuspendUntil = 0;   // stand down after repeated failures

/* market=from_token makes Spotify drop the two `available_markets` arrays
 * (~180 entries each). Those sort before `duration_ms`, so on a truncated
 * read every field we need disappears while the request still returns 200. */
static void syPoll() {
  syPolled = true;
  syPollCount++;
  int code = syRequestTracked("GET",
      "/v1/me/player?additional_types=track&market=from_token", true);
  syLastCode = code;
  if (code == 200) {
    syParsePlayer(syBody);
    Serial.printf("[sy] %d bytes, track='%s' artist='%s' playing=%d %d/%d\n",
                  syBodyLen, syTitle, syArtist, (int)syIsPlaying,
                  syProgressMs, syDurationMs);
    if (syHasTrack) { syErr[0] = 0; syFailStreak = 0; }
    else {
      /* Spotify returns 200 with no `item` when the account has a device but
       * nothing loaded. Saying which it is separates "not playing" from "the
       * parser is broken", which look identical from the outside. */
      bool hasItem = strstr(syBody, "\"item\"") != nullptr;
      bool hasDev  = strstr(syBody, "\"device\"") != nullptr;
      snprintf(syErr, sizeof(syErr), hasItem ? "item unparsed %dB"
                                    : hasDev ? "playing nothing"
                                             : "empty reply %dB", syBodyLen);
      syFailStreak++;
      Serial.println("[sy] parse failed, first 400 bytes:");
      Serial.write((const uint8_t *)syBody, min(400, syBodyLen));
      Serial.println();
    }
  }
  else if (code == 204 || code == 404) {
    /* 404 here is Spotify's NO_ACTIVE_DEVICE: the account has no player
     * ready. It resolves itself once something starts playing, which is why
     * transport calls began working "after a while". */
    syHasTrack = false; syIsPlaying = false;
    snprintf(syErr, sizeof(syErr), "no active device");
    syFailStreak++;
  }
  else if (code > 0) {
    char reason[40] = "";
    if (syBodyLen > 0) cjStr(syBody, nullptr, "message", reason, sizeof(reason));
    if (reason[0]) snprintf(syErr, sizeof(syErr), "%d %s", code, reason);
    else           snprintf(syErr, sizeof(syErr), "HTTP %d", code);
    syFailStreak++;
  }
  else {
    // Do NOT clobber a specific reason (low heap, TLS begin) with a generic
    // one -- that is what disguised the heap guard below as "no connection".
    if (!syErr[0]) snprintf(syErr, sizeof(syErr), "no connection");
    syFailStreak++;
  }
  sySuspendUntil = (syFailStreak >= 4) ? millis() + 60000 : 0;
}

static void syService() {
  if (!syActive || !syAuthorized() || !wifiOnline()) return;
  // Cap at 2x: a 32s gap between attempts made failures look like idleness.
  /* After repeated failures stand down for a minute. Each attempt blocks
   * netTask for seconds, and the speaker shares that task, so a persistently
   * broken Spotify link would otherwise take the volume knob with it. */
  if (sySuspendUntil && (int32_t)(millis() - sySuspendUntil) < 0) return;
  uint32_t iv = (uint32_t)spCfgSyPoll << min(syFailStreak, (uint32_t)1);
  if (!elapsed(syLastPoll, iv)) return;
  syLastPoll = millis();
  netPollSy = true;
}

/* After a local pause or scrub, our own progress clock is more accurate than
 * a poll that may still describe the pre-change state, so ignore the server's
 * progress and playing flag for a moment. */
static uint32_t syLocalMs = 0, syLocalLen = 0;
static bool syLocalActive() { return syLocalLen && !elapsed(syLocalMs, syLocalLen); }
static void syHoldLocal(uint32_t ms) { syLocalMs = millis(); syLocalLen = ms; }

static void syPlay()  { syRequest("PUT", "/v1/me/player/play", false); }
static void syPause() { syRequest("PUT", "/v1/me/player/pause", false); }
static void sySeek(int ms) {
  char path[64];
  snprintf(path, sizeof(path), "/v1/me/player/seek?position_ms=%d", ms < 0 ? 0 : ms);
  syRequest("PUT", path, false);
}

static void syNext()    { syRequest("POST", "/v1/me/player/next", false);     syLastPoll = 0; }
static void syPrev()    { syRequest("POST", "/v1/me/player/previous", false); syLastPoll = 0; }
static void syPlayPause() {
  syRequest("PUT", syIsPlaying ? "/v1/me/player/pause" : "/v1/me/player/play", false);
  syIsPlaying = !syIsPlaying;      // optimistic, corrected on the next poll
  syLastPoll = 0;
}

static void syLogout() {
  syRefresh[0] = syAccess[0] = 0;
  syHasTrack = false;
  syTitle[0] = syArtist[0] = 0;
  nvTouch();
  nvFlush();
}

/* ###########################################################################
 * #                 SPOTIFY SETUP  (built-in config page)                   #
 * ######################################################################### */

static WebServer *syWeb = nullptr;
static char sySetupMsg[120] = "";

static String syHtml() {
  String h = F("<!doctype html><meta name=viewport content='width=device-width'>"
               "<style>body{font-family:sans-serif;margin:2em;max-width:38em}"
               "input{width:100%;padding:.5em;margin:.3em 0}"
               "button{padding:.6em 1.2em}code{background:#eee;padding:.2em}</style>"
               "<h2>Knob OS &mdash; Spotify</h2>");
  h += "<p><b>Status:</b> ";
  h += syAuthorized() ? "authorised" : (syConfigured() ? "credentials saved, not authorised" : "not configured");
  if (sySetupMsg[0]) { h += "<br><b>"; h += sySetupMsg; h += "</b>"; }
  if (syRaw[0]) { h += "<br><small>Last reply: <code>"; h += syRaw; h += "</code></small>"; }
  h += "</p><h3>1. Credentials</h3>"
       "<p>In the Spotify developer dashboard add this exact redirect URI:<br>"
       "<code>http://127.0.0.1:8888/callback</code></p>"
       "<form method=POST action=/save>"
       "<label>Client ID</label><input name=id value='";
  h += syClientId;
  h += "'><label>Client Secret</label><input name=secret value='";
  h += sySecret[0] ? "(unchanged)" : "";
  h += "'><button>Save</button></form>";

  if (syConfigured()) {
    String url = String("https://accounts.spotify.com/authorize?client_id=") + syClientId +
                 "&response_type=code&redirect_uri=http%3A%2F%2F127.0.0.1%3A8888%2Fcallback"
                 "&scope=" + SY_SCOPE_ENC;
    h += "<h3>2. Authorise</h3><p><a target=_blank href='" + url + "'>Open Spotify authorisation</a>"
         "<br>Your browser will end up at a <i>127.0.0.1</i> page that fails to load. "
         "That is expected &mdash; copy the <code>code=</code> value out of the address bar.</p>"
         "<h3>3. Paste the code</h3><form method=POST action=/code>"
         "<input name=code placeholder='paste code here'><button>Exchange</button></form>";
  }
  h += "<h3>Reset</h3><form method=POST action=/logout><button>Log out</button></form>";
  return h;
}

static void syWebRoot() { syWeb->send(200, "text/html", syHtml()); }

static void syWebSave() {
  String id = syWeb->arg("id"), sec = syWeb->arg("secret");
  id.trim(); sec.trim();
  if (id.length()) snprintf(syClientId, sizeof(syClientId), "%s", id.c_str());
  if (sec.length() && sec != "(unchanged)")
    snprintf(sySecret, sizeof(sySecret), "%s", sec.c_str());
  nvTouch(); nvFlush();
  snprintf(sySetupMsg, sizeof(sySetupMsg), "Credentials saved.");
  syWeb->sendHeader("Location", "/");
  syWeb->send(303);
}

static void syWebCode() {
  String c = syWeb->arg("code");
  c.trim();
  if (!c.length()) snprintf(sySetupMsg, sizeof(sySetupMsg), "No code supplied.");
  else if (syExchangeCode(c)) snprintf(sySetupMsg, sizeof(sySetupMsg), "Authorised.");
  else snprintf(sySetupMsg, sizeof(sySetupMsg), "Failed: %s", syErr);
  syWeb->sendHeader("Location", "/");
  syWeb->send(303);
}

static void syWebLogout() {
  syLogout();
  snprintf(sySetupMsg, sizeof(sySetupMsg), "Logged out.");
  syWeb->sendHeader("Location", "/");
  syWeb->send(303);
}

static void sySetupEnter() {
  knobInputMode();
  sySetupMsg[0] = 0;
  if (!syWeb) syWeb = new WebServer(80);
  syWeb->on("/", HTTP_GET, syWebRoot);
  syWeb->on("/save", HTTP_POST, syWebSave);
  syWeb->on("/code", HTTP_POST, syWebCode);
  syWeb->on("/logout", HTTP_POST, syWebLogout);
  syWeb->begin();
}

static void sySetupExit() {
  // Free it rather than just stopping: the TLS handshake needs the heap.
  if (syWeb) { syWeb->stop(); delete syWeb; syWeb = nullptr; }
}
static void sySetupTick() { if (syWeb) syWeb->handleClient(); }

static void sySetupDraw() {
  uiWide();
  drawTitle("Spotify Setup");
  if (!wifiOnline()) {
    display.setCursor(0, 26);
    display.print("Wi-Fi offline");
    return;
  }
  display.setCursor(0, 14);
  display.print("Browse to:");

  char url[32];
  snprintf(url, sizeof(url), "http://%s", WiFi.localIP().toString().c_str());
  printClipped(0, 24, url, CONTENT_W);      // fits on one line at full width

  display.setCursor(0, 36);
  display.print(syAuthorized() ? "authorised" :
                (syConfigured() ? "creds saved" : "not set up"));

  // Wrapped, not clipped, and clear of the status row above.
  const char *m = sySetupMsg[0] ? sySetupMsg : "waiting...";
  int cols = uiCols();
  for (int i = 0, row = 0; m[i] && row < 2; i += cols, row++)
    printClipped(0, 47 + row * 9, m + i, CONTENT_W);
}

static const App AppSpotifySetup = {
  "Spotify Setup", nullptr, sySetupEnter, sySetupExit, sySetupTick, sySetupDraw,
  nullptr, nullptr, nullptr, nullptr
};

/* ---- Spotify settings page --------------------------------------------- */
static const char *const OPT_TRANSPORT[] = { "Spotify Web", "Sony local", "Off" };

static void syFmtStatus(char *b, size_t n) {
  snprintf(b, n, "%s", syAuthorized() ? "OK" : (syConfigured() ? "no auth" : "unset"));
}
static void syActLogout() { syLogout(); }
static void syActRefresh() { syAccess[0] = 0; syLastPoll = 0; syRefreshAccess(); }
// Force a metadata poll regardless of the scheduler, for diagnosis.
static void syActPollNow() { syFailStreak = 0; syLastPoll = 0; netPollSy = true; }

static const CfgItem SPOTIFY_ITEMS[] = {
  ITEM_LINKF("Setup...",   AppSpotifySetup, syFmtStatus),
  ITEM_ENUM ("Transport",  spCfgTransport, OPT_TRANSPORT),
  ITEM_INT  ("Meta Poll",  spCfgSyPoll, 2000, 30000, 500, "ms"),
  ITEM_ACT  ("Force Token", syActRefresh),
  ITEM_ACT  ("Poll Now",    syActPollNow),
  ITEM_ACT  ("Log out",     syActLogout),
};
static CfgPage PageSpotify = { "Spotify", SPOTIFY_ITEMS, 6, 0, false, nullptr };
static void syMenuEnter() { cfgOpen(&PageSpotify); }

static const App AppSpotifyMenu = {
  "Spotify", nullptr, syMenuEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, nullptr
};

/* ---- Playback: cue/review scrubbing and clock format -------------------- */
static const char *const OPT_TIMER[] = { "Remaining", "Total" };

static void fmtSeekHold(char *b, size_t n) {
  if (!spCfgSeekHold) snprintf(b, n, "Off");
  else snprintf(b, n, "%ldms", (long)spCfgSeekHold);
}
static void fmtSeekAccel(char *b, size_t n) {
  if (!spCfgSeekAccel) snprintf(b, n, "Off");
  else snprintf(b, n, "%ld%%/s", (long)spCfgSeekAccel);
}
static void fmtSeekRate(char *b, size_t n) {
  snprintf(b, n, "%lds/s", (long)spCfgSeekRate);
}

static const CfgItem PLAYBACK_ITEMS[] = {
  { "Hold",  C_INT, &spCfgSeekHold,  0, 2000, 50, nullptr,0, nullptr,nullptr, fmtSeekHold,  nullptr },
  { "Speed", C_INT, &spCfgSeekRate,  1,  120,  1, nullptr,0, nullptr,nullptr, fmtSeekRate,  nullptr },
  ITEM_INT ("Step",  spCfgSeekStep,  20, 2000, 10, "ms"),
  { "Accel", C_INT, &spCfgSeekAccel, 0,  400, 10, nullptr,0, nullptr,nullptr, fmtSeekAccel, nullptr },
  ITEM_ENUM("Right", spCfgTimeRight, OPT_TIMER),
};
static CfgPage PagePlayback = { "Playback", PLAYBACK_ITEMS, 5, 0, false, nullptr };
static void pbEnter() { cfgOpen(&PagePlayback); }

static const App AppPlayback = {
  "Playback", nullptr, pbEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, nullptr
};

/* ###########################################################################
 * #                     SPEAKER SETTINGS PAGE                               #
 * ######################################################################### */

static const char *spsTitle() {
  if (!spkIpSet())  return syAuthorized() ? "Speaker: None" : "Speaker: Setup";
  if (spkLinkOk())  return "Speaker: Ready";
  return "Speaker: Offline";
}

static void fmtSpeakerIp(char *b, size_t n) {
  if (!spkIpSet()) snprintf(b, n, "Setup");
  else snprintf(b, n, "%s", spkIp().toString().c_str());
}

static const char *const OPT_CENTER[] = { "Auto", "Mute", "Play/Pause" };
static const char *const OPT_BOTTOM[] = { "Show", "Hide" };

static void spsScanLink() { navPush(&AppSonyScan); }
static void spsIpEdit() {
  ipInputBegin(&spCfgIp, "Speaker IP", spkAfterConfigured);
  gIpAllowDemo = false;
  gIpDemo = nullptr;
  navPush(&AppIpInput);
}

static const CfgItem SPEAKER_ITEMS[] = {
  ITEM_ACT ("Scan...",      spsScanLink),
  ITEM_ACTF("Manual IP",    spsIpEdit, fmtSpeakerIp),
  ITEM_LINK("Speaker info", AppSpeakerInfo),
  ITEM_INT ("Poll",      spCfgPollMs,   200, 3000, 50, "ms"),
  ITEM_INT ("State Poll",spCfgStateMs,  500, 8000,100, "ms"),
  ITEM_INT ("Send Gap",  spCfgSendMs,    50, 1000, 10, "ms"),
  ITEM_INT ("User Hold", spCfgUserHold, 100, 3000, 50, "ms"),
  ITEM_INT ("Vol Min",   spCfgVolMin,     0,  100,  1, ""),
  ITEM_INT ("Vol Max",   spCfgVolMax,     1,  100,  1, ""),
  ITEM_ENUM("Centre Btn", spCfgCenterBtn, OPT_CENTER),
  ITEM_ENUM("Progress",   spCfgBottom, OPT_BOTTOM),
  ITEM_INT ("Scroll",     spCfgScroll,   50, 500,50, "ms"),
  ITEM_LINK("Spotify",    AppSpotifyMenu),
  ITEM_LINK("Playback",   AppPlayback),
};
static CfgPage PageSpeaker = { "Speaker", SPEAKER_ITEMS, 14, 0, false, spsTitle };
static void spsEnter() { cfgOpen(&PageSpeaker); }

// Entered as a prerequisite gate, backing out returns home, matching Wi-Fi.
static bool spsBack() {
  if (gSpkGateway) {
    gSpkGateway = false;
    gAfterConnect = nullptr;
    navHome();
    return true;
  }
  return false;
}

static const App AppSpotifySettings = {
  "Speaker", nullptr, spsEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, spsBack
};

/* ###########################################################################
 * #                     BACKGROUND NETWORK TASK                             #
 * ######################################################################### */

static void netTask(void *) {
  for (;;) {
    bool did = false;
    if (spkActive || syActive) {
      int v = netVolWrite;
      if (v >= 0) {
        netVolWrite = -1; netBusy = true; did = true;
        /* A failed write used to be dropped, and spaTick would then adopt the
         * speaker's unchanged value and quietly undo the user's detent. That
         * is the occasional "turned it and nothing happened". Requeue it. */
        if (!spkSetVolume(v) && netVolRetry > 0) { netVolRetry--; netVolWrite = v; }
      } else if (netCmd) {
        uint8_t c = netCmd; netCmd = 0; netBusy = true; did = true;
        if (c == 1)      spkNext();
        else if (c == 2) spkPrev();
        else if (c == 3) spkPlayPause();
        else if (c == 4) spkSetMute(!spkMute);
      } else if (netPollVol || netPollState) {
        /* Round-robin rather than fixed priority. A strict chain let a slow,
         * frequently-scheduled poll at the top permanently crowd out the one
         * below it. Starting the search at a rotating cursor gives every
         * request a turn regardless of how long its neighbours take. */
        for (int i = 0; i < 2 && !did; i++) {
          int slot = (netRR + i) % 2;
          if (slot == 0 && netPollVol) {
            netPollVol = false; netBusy = true; spkPollVolume(); did = true;
          } else if (slot == 1 && netPollState) {
            netPollState = false; netBusy = true; spkPollState(); did = true;
          }
          if (did) netRR = (slot + 1) % 3;
        }
      }
      netBusy = false;
    } else {
      // (TLS lifetime is syTask's business now)
    }
    vTaskDelay(pdMS_TO_TICKS(did ? 5 : 25));
  }
}

/* Spotify runs on its own task.
 *
 * This is the fix for a tension I kept trading back and forth: a TLS
 * handshake genuinely needs several seconds, but netTask is serial, so any
 * timeout long enough for Spotify also stalled the speaker's volume writes
 * behind it. Shortening the timeout fixed the speaker and broke Spotify;
 * lengthening it did the reverse. They are simply different workloads -- one
 * LAN-fast, one cloud-slow -- and they needed separate tasks rather than a
 * compromise timeout that suits neither. Each already has its own mutex. */
static void syTask(void *) {
  for (;;) {
    bool did = false;
    if (syActive) {
      uint8_t c = syCmd;
      if (c) {
        syCmd = 0; did = true;
        if (c == 1)      syNext();
        else if (c == 2) syPrev();
        else if (c == 5) syPlay();
        else if (c == 6) syPause();
        else if (c == 7) sySeek(netSeekMs);
        syCmdPending = false;
      } else if (netPollSy) {
        netPollSy = false; did = true; syPoll();
      }
      // Below the ~60s most servers allow before closing an idle connection.
      if (!did) syTlsIdle(45000);
    } else {
      syTlsIdle(0);          // app closed: release the session promptly
    }
    vTaskDelay(pdMS_TO_TICKS(did ? 5 : 25));
  }
}

/* ###########################################################################
 * #                   SPEAKER CONTROL MINI-APP                              #
 * ######################################################################### */

extern const App AppSpeaker;

// Filled rounded square rather than a disc, so it sits inside the tile border
// instead of floating in it. The three arcs are struck out in black.
/* Pre-rendered Spotify mark, 27x27, 1bpp MSB-first (108 bytes).
 * Generated offline: rounded square with three shallow constant-thickness
 * arcs and rounded end caps. Drawing this at runtime from circle maths gave
 * either a WiFi-fan look (too steep) or 1px stepping artefacts at the apex,
 * so the raster is baked in. Regenerate with tools/gen_icon.py if resized. */
static const uint8_t SPOTIFY_BMP[] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xFF, 0xFC, 0x00,
  0x1F, 0xFF, 0xFF, 0x00, 0x1F, 0xFF, 0xFF, 0x00, 0x3F, 0xFF, 0xFF, 0x80,
  0x3F, 0xFF, 0xFF, 0x80, 0x3F, 0xFF, 0xFF, 0x80, 0x3F, 0x00, 0x1F, 0x80,
  0x38, 0x00, 0x03, 0x80, 0x30, 0xFF, 0xE1, 0x80, 0x3F, 0xFF, 0xFF, 0x80,
  0x3F, 0xC0, 0x7F, 0x80, 0x3C, 0x00, 0x07, 0x80, 0x3C, 0x3F, 0x87, 0x80,
  0x3F, 0xFF, 0xFF, 0x80, 0x3F, 0xE0, 0xFF, 0x80, 0x3F, 0x00, 0x1F, 0x80,
  0x3F, 0x0E, 0x1F, 0x80, 0x3F, 0xFF, 0xFF, 0x80, 0x3F, 0xFF, 0xFF, 0x80,
  0x3F, 0xFF, 0xFF, 0x80, 0x1F, 0xFF, 0xFF, 0x00, 0x1F, 0xFF, 0xFF, 0x00,
  0x07, 0xFF, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void iconSpotify(int x, int y, int s) {
  // The raster is fixed at 27px; centre it if the box is ever a different size.
  display.drawBitmap(x + (s - 27) / 2, y + (s - 27) / 2, SPOTIFY_BMP, 27, 27,
                     SH110X_WHITE);
}

/* Volume model
 *   spaTarget = what the user has dialled in (updates instantly on screen)
 *   spaSent   = last value actually written to the speaker
 *   spkVol    = last value the speaker reported back
 * Writes are coalesced: detents accumulate into spaTarget and only reach the
 * speaker once the knob has been still for Send Gap, so a fast spin becomes
 * one request rather than thirty.                                          */
static int      spaTarget = -1, spaSent = -1;
static bool     spaUserActive = false, spaPending = false;
static uint32_t spaChangeMs = 0, spaSendMs = 0, spaUserMs = 0, spaPendMs = 0;
static bool     spaSeeking = false, spaCPrev = false, spaAPrev = false;
static bool     spaHoldFired = false;

static uint8_t  spaSeekBtn = 0;                 // 1 = forward, 2 = back
static uint32_t spaSeekStart = 0, spaSeekLast = 0;

static void spaEnter() {
  knobResetSteps();
  spkActive = true;
  syActive = true;
  spkQuiet(0);
  spkLastVolPoll = spkLastStatePoll = 0;
  syLastPoll = 0;
  // Queue the first metadata poll directly rather than waiting for
  // syService() to schedule it, so entry never depends on that gate.
  netPollVol = netPollState = true;
  if (syAuthorized()) netPollSy = true;
  spaTarget = spaSent = spkVol;
  spaPending = spaUserActive = false;
  spaHoldFired = false;
}

static void spaExit() { spkActive = false; syActive = false; }

/* Volume is now a plain accumulator.
 *
 * With a relative encoder there is no physical position to reconcile, so the
 * whole sync apparatus the servo needed -- park, adopt, motion gate, anchor,
 * two-poll confirmation -- is simply gone. A remote change overwrites the
 * value; the next detent moves on from there. Nothing can disagree.        */
/* Ring output for the speaker: the volume within the configured window, so
 * a full ring means Vol Max rather than the device's absolute maximum. */
static void spaRing() {
  if (!npEnable || npSpMode == 0) return;
  npFrameStart();
  if (spkIpSet()) {
    int shown = (spaTarget >= 0) ? spaTarget : spkVol;
    npScale(spkVolToFrac(shown), npSpCol, false,
            npSpEnds != 0, npSpWarnCol, false, false);
  } else if (syDurationMs > 0) {
    // No speaker: show track progress instead of an empty ring.
    npScale((float)syProgressNow() / syDurationMs, npSpCol, false,
            false, 0, false, false);
  }
  npFrameEnd();
}

static void spaTick() {
  // ---- knob ------------------------------------------------------------
  int st = spkIpSet() ? knobStepsFast() : 0;
  if (st) {
    spaTarget = constrain(spaTarget + st, spkUserMin(), spkUserMax());
    spaChangeMs = spaUserMs = millis();
    spaUserActive = true;
    spkQuiet(900);
  }
  if (spaUserActive && elapsed(spaUserMs, (uint32_t)spCfgUserHold))
    spaUserActive = false;

  // ---- coalesced write -------------------------------------------------
  bool turning = spaUserActive || !elapsed(spaUserMs, 150);
  if (spkIpSet() && !turning && spaTarget != spaSent && netVolWrite < 0 &&
      elapsed(spaChangeMs, (uint32_t)spCfgSendMs)) {
    netVolWrite = spaTarget;          // handed to netTask, returns immediately
    netVolRetry = 2;
    spaSent = spaTarget;
    spaSendMs = millis();
    spaPending = true;
    spaPendMs = millis();
    spkQuiet(500);
  }

  spkService();
  syService();

  if (spaPending) {
    if (spkVol == spaSent) spaPending = false;
    else if (elapsed(spaPendMs, 2500)) spaPending = false;
  }

  // ---- remote change: just adopt it ------------------------------------
  if (!turning && !spaPending && spkIpSet() && spkVol != spaTarget) {
    spaTarget = spaSent = spkVol;
  }

  // ---- shaft button ----------------------------------------------------
  if (spCfgEncBtn == 1) {            // Mute is hold-only, for safety
    if (btnIsDown(B_ENC)) {
      if (!spaHoldFired && btnHeldFor(B_ENC) >= (uint32_t)encCfgHoldMs) {
        spaHoldFired = true;
        if (spkIpSet()) { netCmd = 4; spkQuiet(400); }
      }
    } else spaHoldFired = false;
  }

  // ---- press-and-hold scrubbing ----------------------------------------
  bool cD = btnIsDown(B_C), aD = btnIsDown(B_A);
  if (spaSeekArmed()) {
    if (spaSeeking) {
      if (!((spaSeekBtn == 1) ? cD : aD)) spaSeekEnd();
      else spaSeekAdvance();
    } else if (cD && btnHeldFor(B_C) >= (uint32_t)spCfgSeekHold) spaSeekBegin(1);
    else if (aD && btnHeldFor(B_A) >= (uint32_t)spCfgSeekHold) spaSeekBegin(2);
    else {
      // A button pulled into the back chord looks like a release; ignore it.
      if (!cD && spaCPrev && !btnSuppressed(B_C)) { syCmd = 1; syMarkPending(); }
      if (!aD && spaAPrev && !btnSuppressed(B_A)) { syCmd = 2; syMarkPending(); }
    }
  }
  spaCPrev = cD;
  spaAPrev = aD;

  spaRing();
}

static bool spaIsPlaying() {
  return (spCfgTransport == 0) ? syIsPlaying : spkPlaying();
}

// 0 = Auto: play/pause when Spotify can actually service it, else mute.
static int spaCentreMode() {
  bool canPlay = (spCfgTransport == 0 && syAuthorized()) ||
                 (spCfgTransport == 1 && spkIpSet());
  if (!spkIpSet() && canPlay) return 2;      // mute impossible; offer play
  if (spCfgCenterBtn != 0) return spCfgCenterBtn;
  return canPlay ? 2 : 1;
}

// Scrubbing only makes sense when we know the track and its length.
static bool spaSeekArmed() {
  return spCfgSeekHold > 0 && spCfgTransport == 0 && syHasTrack && syDurationMs > 0;
}

// Route a transport request to whichever backend is configured.
static void spaSendNext() {
  if (spCfgTransport == 0) { syCmd = 1; syMarkPending(); }
  else if (spCfgTransport == 1) netCmd = 1;
}
static void spaSendPrev() {
  if (spCfgTransport == 0) { syCmd = 2; syMarkPending(); }
  else if (spCfgTransport == 1) netCmd = 2;
}

static void spaSeekBegin(uint8_t which) {
  spaSeeking = true;
  spaSeekBtn = which;
  spaSeekStart = spaSeekLast = millis();
  syProgressMs = syProgressNow();
  syFetchMs = millis();
  syHoldLocal(120000);          // our clock is authoritative while scrubbing
}

static void spaSeekAdvance() {
  if (!elapsed(spaSeekLast, (uint32_t)spCfgSeekStep)) return;
  uint32_t now = millis();
  float dt = (now - spaSeekLast) / 1000.0f;
  spaSeekLast = now;
  float held = (now - spaSeekStart) / 1000.0f;
  float spd = (float)spCfgSeekRate;
  if (spCfgSeekAccel > 0) spd *= powf(1.0f + spCfgSeekAccel / 100.0f, held);
  spd = min(spd, (float)spCfgSeekRate * 20.0f);
  int d = (int)(spd * dt * 1000.0f);
  syProgressMs = constrain(syProgressMs + (spaSeekBtn == 1 ? d : -d), 0, syDurationMs);
  syFetchMs = now;
}

// Only the final position is sent; the sweep itself is purely local.
static void spaSeekEnd() {
  spaSeeking = false;
  netSeekMs = syProgressMs;
  syCmd = 7;
  syMarkPending();
  syHoldLocal(3000);
}

static void spaButton(BtnId b, BtnEv e) {
  if (e != EV_PRESS) return;
  // Queued, never called inline: a transport request can take a full second.
  // With scrubbing armed, C and A are decided on release in spaTick instead.
  if (b == B_C) { if (!spaSeekArmed()) { spaSendNext(); } }
  else if (b == B_A) { if (!spaSeekArmed()) { spaSendPrev(); } }
  else if (b == B_B) {
    if (spaCentreMode() != 2) {
      if (!spkIpSet()) return;                 // nothing to mute
      netCmd = 4; spkQuiet(400); return;
    }
    if (spCfgTransport == 0) {
      // Freeze or resume our own clock immediately so the bar reacts at once
      // instead of waiting for Spotify to confirm.
      syProgressMs = syProgressNow();
      syFetchMs = millis();
      syIsPlaying = !syIsPlaying;
      syHoldLocal(2500);
      syCmd = syIsPlaying ? 5 : 6;
      syMarkPending();
    } else netCmd = 3;
  } else if (b == B_ENC) {
    /* Mute (option 1) deliberately does NOT act on a click -- spaTick fires
     * it after a hold instead, so a stray press cannot silence the room. */
    if (spCfgEncBtn == 0) {
      if (spaCentreMode() == 2) {
        if (spCfgTransport == 0) {
          syProgressMs = syProgressNow();
          syFetchMs = millis();
          syIsPlaying = !syIsPlaying;
          syHoldLocal(2500);
          syCmd = syIsPlaying ? 5 : 6;
          syMarkPending();
        } else netCmd = 3;
      }
    } else if (spCfgEncBtn == 2) {
      spaSendNext();
    }
  }
}

/* Layout: top half is the track title, then a quarter for the artist, then a
 * quarter for the progress bar. The lower half swaps to volume while the knob
 * is being turned (or permanently, per the Bottom setting).                 */
// Horizontal scroller for text too long to fit on one row.
/* Stepping from millis() beat against the 50ms frame period, so some frames
 * advanced 2px and others 0. Driving it from the frame counter instead gives
 * exactly one step per N frames. Keep Scroll a multiple of DISPLAY_MS. */
static void drawMarquee(int y, const char *t, int size) {
  int cw = 6 * size;
  int total = (int)strlen(t) * cw + 30;
  if (total < 1) return;
  uint32_t scroll = (uint32_t)(spCfgScroll < 10 ? 10 : spCfgScroll);
  uint32_t per = scroll / DISPLAY_MS;
  if (per < 1) per = 1;
  int off = (int)((gFrame / per) % (uint32_t)total);
  display.setTextSize(size);
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; t[i]; i++) {
      int x = i * cw - off + pass * total;
      if (x <= -cw || x >= CONTENT_W) continue;
      display.setCursor(x, y);
      display.write((uint8_t)t[i]);
    }
  display.setTextSize(1);
}

static void drawRowText(int y, const char *s) {
  if (!s || !s[0]) return;
  int w = (int)strlen(s) * 6;
  if (w <= CONTENT_W) {
    display.setCursor((CONTENT_W - w) / 2, y);
    display.print(s);
  } else drawMarquee(y, s, 1);
}

static void fmtClock(char *b, size_t n, int sec, bool neg) {
  if (sec < 0) sec = 0;
  snprintf(b, n, "%s%d:%02d", neg ? "-" : "", sec / 60, sec % 60);
}

/* Layout: title, artist, progress bar and its clocks up top; volume occupies
 * the bottom third. Gaps are deliberately uneven so each block reads as a
 * group rather than as evenly spaced lines. */
static void spaDraw() {
  int shown = (spaTarget >= 0) ? spaTarget : spkVol;
  shown = constrain(shown, 0, 999);

  /* "No track info" used to cover two very different states: a poll that
   * found nothing, and no poll having run at all. Separating them makes a
   * silent polling failure visible instead of looking like an empty queue. */
  const char *t = syTitle[0] ? syTitle
                : !syAuthorized() ? "Spotify not set up"
                : syErr[0] ? syErr
                : (gSyBusy || !syPolled) ? "Contacting Spotify..."
                : "Spotify idle - no device";
  drawRowText(0, t);

  /* With no track, the artist row is free -- use it for the last HTTP code
   * and body size rather than making the user go and find Speaker info. */
  char dbg[26] = "";
  if (!syTitle[0] && syAuthorized())
    snprintf(dbg, sizeof(dbg), "code %d len %d rssi %d", syLastCode, syBodyLen,
             (int)WiFi.RSSI());

  const char *a2 = syArtist[0] ? syArtist
                 : dbg[0] ? dbg
                 : !wifiOnline() ? "Wi-Fi offline"
                 : (spkIpSet() && !spkLinkOk()) ? "speaker no response"
                 : spkMute ? "MUTED" : "";
  drawRowText(12, a2);

  if (syDurationMs > 0) {
    int pos = syProgressNow();
    drawBar(0, 25, CONTENT_W, 4, (float)pos / syDurationMs);

    /* Both clocks are derived from the same elapsed-second value, so they
     * tick together. Computing them independently made them change at
     * different moments, since the duration is not a whole second. */
    int totalSec = (syDurationMs + 500) / 1000;
    int elapsedSec = min(pos / 1000, totalSec);
    char l[10], r[10];
    fmtClock(l, sizeof(l), elapsedSec, false);
    if (spCfgTimeRight == 1) fmtClock(r, sizeof(r), totalSec, false);
    else fmtClock(r, sizeof(r), totalSec - elapsedSec, true);
    display.setCursor(0, 31);
    display.print(l);
    display.setCursor(CONTENT_W - (int)strlen(r) * 6, 31);
    display.print(r);

    const char *mid = spaSeeking ? ((spaSeekBtn == 1) ? ">>" : "<<")
                    : syPendingNow() ? "..." : nullptr;
    if (mid) {
      display.setCursor((CONTENT_W - (int)strlen(mid) * 6) / 2, 31);
      display.print(mid);
    }
  }

  // ---- bottom: volume, or a note that there is no speaker --------------
  if (!spkIpSet()) {
    const char *msg = "No speaker - Spotify only";
    drawRowText(46, msg);
    display.fillRect(CONTENT_W, 0, SCREEN_W - CONTENT_W, SCREEN_H, SH110X_BLACK);
    Glyph mid2 = (spaCentreMode() == 2) ? (spaIsPlaying() ? G_PAUSE : G_PLAY)
                                        : G_SPEAKER;
    drawButtonLabels(G_NEXT, mid2, G_PREV);
    return;
  }

  char lo[6], hi[6], nv[6];
  snprintf(lo, sizeof(lo), "%d", spkUserMin());
  snprintf(hi, sizeof(hi), "%d", spkUserMax());
  snprintf(nv, sizeof(nv), "%d", shown);
  int loW = (int)strlen(lo) * 6, hiW = (int)strlen(hi) * 6, nvW = (int)strlen(nv) * 6;
  int barX = loW + 4;
  int barW = CONTENT_W - loW - hiW - 8;
  if (barW < 20) { barX = 0; barW = CONTENT_W; }

  display.setTextSize(1);
  display.setCursor(0, 52);
  display.print(lo);
  display.setCursor(CONTENT_W - hiW, 52);
  display.print(hi);
  display.setCursor(barX + (barW - nvW) / 2, 42);
  display.print(nv);
  if (spkMute) { display.setCursor(CONTENT_W - 24, 42); display.print("MUTE"); }
  else if (spaPending) { display.setCursor(CONTENT_W - 18, 42); display.print("..."); }

  drawBar(barX, 52, barW, 8, spkVolToFrac(shown));

  display.fillRect(CONTENT_W, 0, SCREEN_W - CONTENT_W, SCREEN_H, SH110X_BLACK);

  Glyph mid = (spaCentreMode() == 2) ? (spaIsPlaying() ? G_PAUSE : G_PLAY)
                                     : (spkMute ? G_MUTED : G_SPEAKER);
  drawButtonLabels(G_NEXT, mid, G_PREV);
}

static void speakerLaunch() {
  // Wi-Fi is the only hard prerequisite: without it neither half can work.
  if (!wifiConfigured() || !wifiOnline()) {
    gAfterConnect = &AppSpeaker;
    gWifiGateway = true;
    navPush(&AppWifi);
    return;
  }
  // A speaker is needed only if Spotify cannot carry the app on its own.
  if (!spkIpSet() && !syAuthorized()) {
    gAfterConnect = &AppSpeaker;
    gSpkGateway = true;
    navPush(&AppSpotifySettings);
    return;
  }
  navPush(&AppSpeaker);
}

const App AppSpeaker = {
  "Speaker Control", iconSpotify, spaEnter, spaExit, spaTick, spaDraw,
  spaButton, nullptr, speakerLaunch, nullptr
};

/* ###########################################################################
 * #                        COLOUR EDITOR                                    #
 * #                                                                         #
 * #  Six fields in two columns: RGB on the left, HSV on the right. They are #
 * #  two views of one colour, so editing either immediately rewrites the    #
 * #  other -- the active column is highlighted so it is obvious which set   #
 * #  the knob is driving.                                                   #
 * ######################################################################### */

static int32_t *gColTarget = nullptr;
static const char *gColTitle = "Colour";
static int  colField = 0;              // 0..2 = R,G,B   3..5 = H,S,V
static int  colLive = 1;               // 0 all pixels, 1 relevant, 2 off
static int  colR, colG, colB, colH, colS, colV;
static int  colPrevFirst = 0, colPrevLast = NEO_N - 1;

static void colLoadFromTarget() {
  int32_t c = gColTarget ? *gColTarget : 0;
  colR = npR(c); colG = npG(c); colB = npB(c);
  npToHSV(c, &colH, &colS, &colV);
}

static void colStore(int32_t c) {
  if (gColTarget) *gColTarget = c;
  nvTouch();
}

// Whichever column was edited becomes the source; the other is recomputed.
static void colSyncFromRGB() {
  int32_t c = npRGB(colR, colG, colB);
  npToHSV(c, &colH, &colS, &colV);
  colStore(c);
}
static void colSyncFromHSV() {
  int32_t c = npFromHSV(colH, colS, colV);
  colR = npR(c); colG = npG(c); colB = npB(c);
  colStore(c);
}

static void colourEditBegin(const char *title, int32_t *target,
                            int prevFirst, int prevLast) {
  gColTitle = title;
  gColTarget = target;
  colPrevFirst = prevFirst;
  colPrevLast = prevLast;
  colField = 0;
  colLoadFromTarget();
}

static void colEnter() { knobInputMode(); colLoadFromTarget(); }
static void colExit()  { nvFlush(); npClear(); }

static void colAdjust(int steps) {
  // Coarse by default; holding the shaft button drops to single units.
  int mag = btnIsDown(B_ENC) ? 1 : constrain((int)npStep, 1, 50);
  int d = steps * mag;
  switch (colField) {
    case 0: colR = constrain(colR + d, 0, 255); colSyncFromRGB(); break;
    case 1: colG = constrain(colG + d, 0, 255); colSyncFromRGB(); break;
    case 2: colB = constrain(colB + d, 0, 255); colSyncFromRGB(); break;
    case 3: colH = (((colH + d) % 360) + 360) % 360; colSyncFromHSV(); break;
    case 4: colS = constrain(colS + d, 0, 100); colSyncFromHSV(); break;
    case 5: colV = constrain(colV + d, 0, 100); colSyncFromHSV(); break;
  }
}

static void colTick() {
  int st = knobSteps();
  if (st) colAdjust(st);

  if (!npEnable || colLive == 2) { npFrameStart(); npFrameEnd(); return; }
  npFrameStart();
  int32_t c = gColTarget ? *gColTarget : 0;
  int a = (colLive == 0) ? 0 : colPrevFirst;
  int b = (colLive == 0) ? NEO_N - 1 : colPrevLast;
  for (int i = a; i <= b && i < NEO_N; i++) npSet(i, c, 1.0f, false);
  npFrameEnd();
}

static void colDrawField(int idx, int x, int y, const char *lbl, int val) {
  bool sel = (colField == idx);
  if (sel) {
    display.fillRect(x - 1, y - 1, 52, 10, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
  }
  display.setCursor(x, y);
  display.printf("%s %3d", lbl, val);
  display.setTextColor(SH110X_WHITE);
}

static void colDraw() {
  drawTitle(gColTitle);
  bool rgbActive = (colField < 3);

  display.setCursor(2, 13);
  display.print(rgbActive ? ">RGB" : " RGB");
  display.setCursor(58, 13);
  display.print(rgbActive ? " HSV" : ">HSV");

  colDrawField(0, 2, 24, "R", colR);
  colDrawField(1, 2, 35, "G", colG);
  colDrawField(2, 2, 46, "B", colB);
  colDrawField(3, 58, 24, "H", colH);
  colDrawField(4, 58, 35, "S", colS);
  colDrawField(5, 58, 46, "V", colV);

  display.setCursor(2, 57);
  display.printf("%s  %s", btnIsDown(B_ENC) ? "FINE" : "CRSE",
                 colLive == 0 ? "all" : colLive == 1 ? "part" : "off");

  // A swatch of the live colour, drawn as a filled block scaled by value.
  int32_t c = gColTarget ? *gColTarget : 0;
  int lum = (npR(c) * 30 + npG(c) * 59 + npB(c) * 11) / 100;
  display.drawRect(88, 54, 20, 9, SH110X_WHITE);
  if (lum > 20) display.fillRect(90, 56, 16, 5, SH110X_WHITE);

  drawButtonLabels(G_RIGHT, G_SUN, G_LEFT);
}

static void colButton(BtnId b, BtnEv e) {
  if (b == B_C) colField = (colField + 1) % 6;
  else if (b == B_A) colField = (colField + 5) % 6;
  else if (b == B_B && e == EV_PRESS) colLive = (colLive + 1) % 3;
}

static const App AppColourEdit = {
  "Colour", nullptr, colEnter, colExit, colTick, colDraw, colButton, nullptr,
  nullptr, nullptr
};

/* ###########################################################################
 * #                     NEOPIXEL SETTINGS                                   #
 * ######################################################################### */

/* Each colour row needs an opener and a formatter. The macro keeps the dozen
 * of them from becoming a dozen near-identical copy-pastes. The two indices
 * are the pixels lit by the editor's "part" preview mode, so the preview
 * shows roughly where on the ring that colour will actually appear. */
#define NP_COLOR_ROW(fn, title, var, pa, pb)                                  \
  static void fn##Open() {                                                    \
    colourEditBegin(title, &var, pa, pb);                                     \
    navPush(&AppColourEdit);                                                  \
  }                                                                           \
  static void fn##Fmt(char *b_, size_t n_) {                                  \
    snprintf(b_, n_, "%06lX", (unsigned long)(var & 0xFFFFFF));               \
  }

NP_COLOR_ROW(npcMxFad,  "Fader",     npMxFadCol,  0, NEO_N - 1)
NP_COLOR_ROW(npcMxRb,   "Rate B",    npMxRbCol,   0, NEO_N - 1)
NP_COLOR_ROW(npcMxZ1,   "Zone 1",    npMxZ1,      0, 7)
NP_COLOR_ROW(npcMxZ2,   "Zone 2",    npMxZ2,      8, 10)
NP_COLOR_ROW(npcMxZ3,   "Zone 3",    npMxZ3,     11, 11)
NP_COLOR_ROW(npcMxZ4,   "Zone 4",    npMxZ4,     11, 11)
NP_COLOR_ROW(npcMxWarn, "Ends Warn", npMxWarnCol, 0, 0)
NP_COLOR_ROW(npcSpCol,  "Volume",    npSpCol,     0, NEO_N - 1)
NP_COLOR_ROW(npcSpWarn, "Ends Warn", npSpWarnCol, 0, 0)

static const char *const OPT_NP_MXMODE[] = { "Off", "Fader", "Meter" };
static const char *const OPT_NP_SPMODE[] = { "Off", "Volume" };
static const char *const OPT_NP_CLIP[]   = { "Off", "No Dim", "Full Bright" };

/* ---- global ------------------------------------------------------------ */
static const CfgItem NP_GLOBAL_ITEMS[] = {
  ITEM_INT ("Brightness", npBright, 1, 255, 5, ""),
  ITEM_BOOL("Smooth",     npSmooth),
  ITEM_INT ("Origin",     npOrigin, 0, NEO_N - 1, 1, ""),
  ITEM_BOOL("Reverse",    npReverse),
  ITEM_INT ("Colour Step",npStep,   1,  50, 1, ""),
};
static CfgPage PageNpGlobal = { "NeoPixel Global", NP_GLOBAL_ITEMS, 5, 0, false, nullptr };
static void npgEnter() { cfgOpen(&PageNpGlobal); }
static const App AppNpGlobal = {
  "Global", nullptr, npgEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, nullptr
};

/* ---- mixer ------------------------------------------------------------- */
static const CfgItem NP_MIXER_ITEMS[] = {
  ITEM_ENUM("Mode",       npMxMode, OPT_NP_MXMODE),
  ITEM_ACTF("Fader Col",  npcMxFadOpen,  npcMxFadFmt),
  ITEM_ACTF("Rate B Col", npcMxRbOpen,   npcMxRbFmt),
  ITEM_INT ("Zones",      npMxZones, 2, 4, 1, ""),
  ITEM_ACTF("Zone 1",     npcMxZ1Open,   npcMxZ1Fmt),
  ITEM_ACTF("Zone 2",     npcMxZ2Open,   npcMxZ2Fmt),
  ITEM_ACTF("Zone 3",     npcMxZ3Open,   npcMxZ3Fmt),
  ITEM_ACTF("Zone 4",     npcMxZ4Open,   npcMxZ4Fmt),
  ITEM_INT ("Thresh 2",   npMxT2, 1, 99, 1, "%"),
  ITEM_INT ("Thresh 3",   npMxT3, 1, 99, 1, "%"),
  ITEM_INT ("Thresh 4",   npMxT4, 1, 99, 1, "%"),
  ITEM_ENUM("Clip LED",   npMxClip, OPT_NP_CLIP),
  ITEM_BOOL("Mark Ends",  npMxEnds),
  ITEM_ACTF("Warn Col",   npcMxWarnOpen, npcMxWarnFmt),
};
static CfgPage PageNpMixer = { "NeoPixel Mixer", NP_MIXER_ITEMS, 14, 0, false, nullptr };
static void npmEnter() { cfgOpen(&PageNpMixer); }
static const App AppNpMixer = {
  "Mixer", nullptr, npmEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, nullptr
};

/* ---- speaker ----------------------------------------------------------- */
static const CfgItem NP_SPK_ITEMS[] = {
  ITEM_ENUM("Mode",      npSpMode, OPT_NP_SPMODE),
  ITEM_ACTF("Colour",    npcSpColOpen,  npcSpColFmt),
  ITEM_BOOL("Mark Ends", npSpEnds),
  ITEM_ACTF("Warn Col",  npcSpWarnOpen, npcSpWarnFmt),
};
static CfgPage PageNpSpk = { "NeoPixel Speaker", NP_SPK_ITEMS, 4, 0, false, nullptr };
static void npsEnter() { cfgOpen(&PageNpSpk); }
static const App AppNpSpeaker = {
  "Speaker", nullptr, npsEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, nullptr
};

/* ---- root -------------------------------------------------------------- */
/* Direct hardware test. Writes the strip with no reference to npEnable, the
 * per-app modes, the rate limiter or the dirty check, so it separates "the
 * ring or its driver is not working" from "the app logic is not calling it".
 * Brightness is fixed high here deliberately. */
static int npTestMode = 0;
static uint32_t npTestMs = 0;
static int npTestStep = 0;

static void nptEnter() { knobInputMode(); npTestMode = 0; npTestStep = 0; }
static void nptExit()  { ring.clear(); ring.show(); npLit = false; }

static void nptTick() {
  int st = knobSteps();
  if (st) npTestMode = (npTestMode + st) & 3;
  if (!elapsed(npTestMs, 120)) return;
  npTestMs = millis();
  npTestStep = (npTestStep + 1) % NEO_N;

  for (int i = 0; i < NEO_N; i++) {
    uint32_t c = 0;
    switch (npTestMode) {
      case 0: c = (i == npTestStep) ? ring.Color(120, 120, 120) : 0; break;
      case 1: c = ring.Color(120, 0, 0); break;
      case 2: c = ring.Color(0, 120, 0); break;
      default: c = ring.Color(0, 0, 120); break;
    }
    ring.setPixelColor(i, c);
  }
  ring.show();
  npLit = true;
}

static void nptDraw() {
  drawTitle("NeoPixel Test");
  static const char *names[4] = { "Chase", "All red", "All green", "All blue" };
  display.setCursor(0, 16);
  display.print(names[npTestMode]);
  display.setCursor(0, 30);
  display.printf("pin %d  n %d", PIN_NEO, NEO_N);
  display.setCursor(0, 40);
  display.printf("psram %luk tls %s", (unsigned long)(ESP.getPsramSize() / 1024),
                 gTlsPsram ? "ps" : "int");
  display.setCursor(0, 50);
  display.print("turn to change");
  drawButtonLabels(G_NONE, G_NONE, G_NONE);
}

static const App AppNpTest = {
  "Test", nullptr, nptEnter, nptExit, nptTick, nptDraw, nullptr, nullptr,
  nullptr, nullptr
};

static const CfgItem NP_ITEMS[] = {
  ITEM_BOOL("NeoPixel",  npEnable),
  ITEM_LINK("Global",    AppNpGlobal),
  ITEM_LINK("Mixer",     AppNpMixer),
  ITEM_LINK("Speaker",   AppNpSpeaker),
  ITEM_LINK("Test",      AppNpTest),
};
static CfgPage PageNeo = { "NeoPixel", NP_ITEMS, 5, 0, false, nullptr };
static void npnEnter() { cfgOpen(&PageNeo); }
static const App AppNeoPixel = {
  "NeoPixel", nullptr, npnEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, nullptr
};

/* ###########################################################################
 * #                       SYSTEM SETTINGS                                   #
 * ######################################################################### */

static const char *const OPT_KNOBEDIT[] = { "When selected", "Only when open" };
static const char *const OPT_STARTUP[]  = { "Home screen", "Last app", "Last screen" };
static const char *const OPT_BACK[]     = { "A+B", "B+C", "A+C", "A+B+C" };

static void sysReset() { nvFactoryReset(); }

static const char *const OPT_TX_ENC[] = { "Caps Lock", "Confirm", "Off" };

// 0 disables auto-repeat outright, for anyone who only ever wants taps.
static void fmtStuck(char *b, size_t n) {
  if (!sysStuckSec) snprintf(b, n, "Off");
  else snprintf(b, n, "%lds", (long)sysStuckSec);
}

static void fmtRptDly(char *b, size_t n) {
  if (!sysRepeatDly) snprintf(b, n, "Off");
  else snprintf(b, n, "%ldms", (long)sysRepeatDly);
}

static const CfgItem SYS_ITEMS[] = {
  ITEM_ENUM("Knob Edits",  sysKnobEdit, OPT_KNOBEDIT),
  ITEM_ENUM("Startup",     sysStartup,  OPT_STARTUP),
  ITEM_ENUM("Back",        sysBackAct,  OPT_BACK),
  ITEM_BOOL("Knob Invert", sysKnobInvert),
  ITEM_ENUM("Text Btn",    encCfgTextBtn, OPT_TX_ENC),
  { "Rpt Delay", C_INT, &sysRepeatDly, 0, 2000, 50, nullptr,0, nullptr,nullptr, fmtRptDly, nullptr },
  ITEM_INT ("Rpt Rate",    sysRepeatRate, 30, 500,10, "ms"),
  ITEM_INT ("Bright",      sysBright,      1, 255, 5, ""),
  ITEM_INT ("Back Win",    sysComboMs,    40, 400,10, "ms"),
  { "Stuck Cut", C_INT, &sysStuckSec, 0, 120, 1, nullptr,0, nullptr,nullptr, fmtStuck, nullptr },
  ITEM_BOOL("Flip Screen", sysFlip),
  ITEM_ACT ("Factory Reset", sysReset),
};
static CfgPage PageSystem = { "System Settings", SYS_ITEMS, 12, 0, false, nullptr };
static void ssEnter() { cfgOpen(&PageSystem); }

static const App AppSystemSettings = {
  "System Settings", nullptr, ssEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, nullptr
};

/* ###########################################################################
 * #                             ABOUT                                       #
 * ######################################################################### */

/* Clawd, 12x8. Supplied column-major (one byte per column, MSB = top pixel):
 *   00110000 00110000 11111111 10111100 11111111 11111100
 *   11111100 11111111 10111100 11111111 00110000 00110000
 * Transposed here to the row-major, MSB-leftmost order drawBitmap expects.
 * 12 wide pads to 2 bytes per row; the low 4 bits of each pair go unused.
 *      ..########..
 *      ..#.####.#..
 *      ############
 *      ############
 *      ..########..
 *      ..########..
 *      ..#.#..#.#..
 *      ..#.#..#.#..
 * Set bits draw white; clear bits stay black, which gives the dark eyes. */
static const int CLAWD_W = 12, CLAWD_H = 8;
static const uint8_t CLAWD_BMP[] PROGMEM = {
  0x3F, 0xC0,  0x2F, 0x40,  0xFF, 0xF0,  0xFF, 0xF0,
  0x3F, 0xC0,  0x3F, 0xC0,  0x29, 0x40,  0x29, 0x40,
};

static void abEnter() { knobInputMode(); }

// "Knob" + a dial standing in for the O + "S", faux-bold by double-striking.
static void abText(int x, int y, const char *t, int size, int extra) {
  display.setTextSize(size);
  int adv = 6 * size + extra;
  for (int i = 0; t[i]; i++)
    for (int o = 0; o < 2; o++) {          // double-strike = faux bold
      display.setCursor(x + i * adv + o, y);
      display.write((uint8_t)t[i]);
    }
  display.setTextSize(1);
}
static int abTextW(const char *t, int size, int extra) {
  return (int)strlen(t) * (6 * size + extra) - extra + 1;
}

static void abLogo(int ty) {
  const int SZ = 3, EX = 1, GAP = 2, KW = 20;
  int wKnob = abTextW("Knob", SZ, EX);
  int wS    = abTextW("S", SZ, EX);
  int x = (CONTENT_W - (wKnob + GAP + KW + GAP + wS)) / 2;
  if (x < 0) x = 0;

  abText(x, ty, "Knob", SZ, EX);

  // Capitals fill only the top 7 of the 8 rows in a cell, so the optical
  // centre sits a little above the cell centre -- that is the 1px offset.
  int cx = x + wKnob + GAP + KW / 2;
  int cy = ty + (7 * SZ) / 2;
  for (int r = 7; r <= 9; r++) display.drawCircle(cx, cy, r, SH110X_WHITE);
  display.fillRect(cx - 1, cy - 6, 3, 5, SH110X_WHITE);      // pointer
  display.fillCircle(cx, cy, 1, SH110X_WHITE);               // hub

  abText(x + wKnob + GAP + KW + GAP, ty, "S", SZ, EX);
}

static void abDraw() {
  uiWide();
  abLogo(12);                     // 24px tall, centred in the top 3/4

  char ver[12];
  snprintf(ver, sizeof(ver), "v%s", KNOB_OS_VERSION);

  const char *name = "Johannes + ";
  int rightW = (int)strlen(name) * 6 + CLAWD_W;
  if ((int)strlen(ver) * 6 + rightW + 4 > CONTENT_W) {
    name = "Johannes ";                          // drop the + if it will not fit
    rightW = (int)strlen(name) * 6 + CLAWD_W;
  }

  display.setCursor(0, 52);                      // version left
  display.print(ver);
  display.setCursor(CONTENT_W - rightW, 52);     // name + mascot right
  display.print(name);
  display.drawBitmap(CONTENT_W - CLAWD_W, 52, CLAWD_BMP, CLAWD_W, CLAWD_H,
                     SH110X_WHITE);
}

static const App AppAbout = {
  "About", nullptr, abEnter, nullptr, nullptr, abDraw, nullptr, nullptr,
  nullptr, nullptr
};

/* ###########################################################################
 * #              SLEEP MENU, WARNINGS AND BATTERY SETTINGS                  #
 * ######################################################################### */

static int slpSel = 0;   // 0 = Sleep, 1 = Back

static void slpEnter() { knobInputMode(); slpSel = 0; }

static void slpDraw() {
  int soc = batSoc();
  char t[10];
  snprintf(t, sizeof(t), "%d%%", soc);

  /* Fixed columns: bolt, percentage, battery. The bolt's slot is reserved
   * whether or not it is drawn, so nothing shifts sideways when the charger
   * is plugged in or pulled out. */
  /* Same order as the home readout: bolt, battery, percentage. Every slot is
   * reserved whether or not it is drawn and the block is centred as a whole,
   * so plugging the charger in does not shuffle anything sideways. */
  const int GAP = 9, BOLT_W = 12, BATT_W = 30, PCT_W = 4 * 12;
  int bx = (CONTENT_W - (BOLT_W + GAP + BATT_W + GAP + PCT_W)) / 2;
  if (batCharging()) drawBolt(bx, 2, 2);
  drawBattery(bx + BOLT_W + GAP, 5, soc, 2);
  display.setTextSize(2);
  display.setCursor(bx + BOLT_W + GAP + BATT_W + GAP, 4);
  display.print(t);
  display.setTextSize(1);

  char v[12];
  snprintf(v, sizeof(v), "%d.%02dV", batMv() / 1000, (batMv() % 1000) / 10);
  display.setCursor((CONTENT_W - (int)strlen(v) * 6) / 2, 22);
  display.print(v);

  /* Two buttons inside CONTENT_W with the label column reserved: 4..50 and
   * 56..102, ending at y=48 so the hint line below is not clipped. */
  const char *lbl[2] = { "Sleep", "Off" };
  for (int i = 0; i < 2; i++) {
    int x = 4 + i * 52, y = 32, w = 46, h = 16;
    bool sel = (slpSel == i);
    if (sel) {
      display.fillRoundRect(x, y, w, h, 4, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
    } else {
      display.drawRoundRect(x, y, w, h, 4, SH110X_WHITE);
      display.setTextColor(SH110X_WHITE);
    }
    int tw = (int)strlen(lbl[i]) * 6;
    display.setCursor(x + (w - tw) / 2, y + 5);
    display.print(lbl[i]);
    display.setTextColor(SH110X_WHITE);
  }

  display.setCursor(0, 54);
  display.print(slpSel == 0 ? "wake on any button"
                            : "wake on knob press");
  drawButtonLabels(G_NONE, G_SELECT, G_LEFT);
}

static void slpKnob(int st) { if (st) slpSel = constrain(slpSel + (st > 0 ? 1 : -1), 0, 1); }

static void slpButton(BtnId b, BtnEv e) {
  if (e != EV_PRESS) return;
  if (b == B_A) { navBack(); return; }             // bottom button is back
  if (btnSelect(b)) {
    // Shut Down always uses deep sleep, whatever the default mode is: it is
    // the "put it away" option, so the lowest draw is what is wanted.
    if (slpSel == 0) enterSleep(true, false);
    else             enterSleep(true, true);
  }
}

static const App AppSleepMenu = {
  "Sleep", nullptr, slpEnter, nullptr, nullptr, slpDraw, slpButton, slpKnob,
  nullptr, nullptr
};

/* ---- low battery warning ------------------------------------------------ */
static int gWarnPct = 0;

static void bwEnter() { knobInputMode(); }
static void bwDraw() {
  uiWide();
  int soc = batSoc();
  if (batCharging()) drawBolt(0, 11, 2);
  drawBattery(batCharging() ? 14 : 8, 12, soc, 2);
  display.setTextSize(2);
  display.setCursor(46, 13);
  display.printf("%d%%", gWarnPct);
  display.setTextSize(1);
  display.setCursor(0, 40);
  display.print("Low battery");
  display.setCursor(0, 52);
  display.print("Any button to dismiss");
}
static void bwButton(BtnId b, BtnEv e) {
  (void)b;
  if (e == EV_PRESS) navBack();
}
static const App AppBattWarn = {
  "Low Battery", nullptr, bwEnter, nullptr, nullptr, bwDraw, bwButton, nullptr,
  nullptr, nullptr
};

/* ---- charge-wait mode --------------------------------------------------- */
static void cwaitDraw() {
  uiWide();
  int soc = batSoc();
  drawBolt(0, 9, 2);
  drawBattery(14, 10, soc, 2);
  display.setTextSize(2);
  display.setCursor(46, 11);
  display.printf("%d%%", soc);
  display.setTextSize(1);
  display.setCursor(0, 36);
  display.print("Charging");
  display.setCursor(0, 47);
  display.printf("Resumes at %ld%%", (long)batResume);
  display.setCursor(0, 57);
  display.print("Radio off to charge faster");
}
static const App AppChargeWait = {
  "Charging", nullptr, nullptr, nullptr, nullptr, cwaitDraw, nullptr, nullptr,
  nullptr, nullptr
};

/* ---- settings ----------------------------------------------------------- */
static const char *const OPT_SLEEPMODE[] = { "Light (all btn)", "Deep (shaft)" };

static void fmtSleepMin(char *b, size_t n) {
  if (!batSleepMin) snprintf(b, n, "Never");
  else snprintf(b, n, "%ld min", (long)batSleepMin);
}
static void fmtBattV(char *b, size_t n) {
  snprintf(b, n, "%d.%02dV", batMv() / 1000, (batMv() % 1000) / 10);
}
static void batActSleep() { enterSleep(false); }

static const CfgItem BATT_ITEMS[] = {
  { "Sleep After", C_INT, &batSleepMin, 0, 120, 1, nullptr,0, nullptr,nullptr, fmtSleepMin, nullptr },
  ITEM_ENUM("Sleep Mode", batSleepMode, OPT_SLEEPMODE),
  ITEM_ACT ("Sleep Now",  batActSleep),
  ITEM_ACTF("Voltage",    batActSleep, fmtBattV),
  ITEM_INT ("Batt Ratio", batRatio,  100, 500, 1, "/100"),
  ITEM_INT ("0% mV",      batMv0,   2800, 3800, 10, ""),
  ITEM_INT ("100% mV",    batMv100, 3900, 4300, 10, ""),
  ITEM_INT ("Warn 1",     batWarnHi,   0, 100, 1, "%"),
  ITEM_INT ("Warn 2",     batWarnLo,   0, 100, 1, "%"),
  ITEM_INT ("Resume At",  batResume,   1,  50, 1, "%"),
  ITEM_BOOL("Show in Mixer", batShowMixer),
  ITEM_INT ("Avg Window", batAvgSec,   1, 120, 1, "s"),
  ITEM_INT ("Update Ivl", batUpdSec,   1,  60, 1, "s"),
};
static CfgPage PageBatt = { "Battery+Sleep", BATT_ITEMS, 13, 0, false, nullptr };
static void batEnter() { cfgOpen(&PageBatt); }
static const App AppBattery = {
  "Battery+Sleep", nullptr, batEnter, nullptr, nullptr, cfgDraw, cfgButton,
  cfgKnob, nullptr, nullptr
};

/* Battery supervision, run once per loop.
 *
 * Warnings latch so each threshold fires once per discharge; recharging past
 * the threshold rearms them. */
static void batSupervise() {
  if (batChargeWait) {
    if (batSoc() >= (int)batResume) {
      // Enough charge to run properly. Restart rather than trying to bring
      // radios and apps up from a half-initialised state.
      rtcWokeEmpty = 0;
      delay(50);
      ESP.restart();
    }
    return;
  }

  int soc = batSoc();
  bool chg = batCharging();

  if (chg) { batWarned = 0; batNoteActivity(); }   // charging is not idle

  if (!chg && soc <= 0) { enterEmptySleep(); return; }

  if (!chg && cur() != &AppBattWarn) {
    if (batWarned < 1 && soc <= (int)batWarnHi && soc > (int)batWarnLo) {
      batWarned = 1; gWarnPct = soc; navPush(&AppBattWarn);
    } else if (batWarned < 2 && soc <= (int)batWarnLo) {
      batWarned = 2; gWarnPct = soc; navPush(&AppBattWarn);
    }
  }

  if (batSleepMin > 0 && !chg &&
      elapsed(gLastActivity, (uint32_t)batSleepMin * 60000UL))
    enterSleep(false);
}

/* ###########################################################################
 * #                        SETTINGS ROOT                                    #
 * ######################################################################### */

static void iconGear(int x, int y, int s) {
  int cx = x + s / 2, cy = y + s / 2;
  display.drawCircle(cx, cy, 8, SH110X_WHITE);
  display.drawCircle(cx, cy, 3, SH110X_WHITE);
  const int dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
  const int dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
  for (int i = 0; i < 8; i++)
    display.fillRect(cx + dx[i] - 1, cy + dy[i] - 1, 3, 3, SH110X_WHITE);
}

static const CfgItem SETTINGS_ITEMS[] = {
  ITEM_LINK("System Settings", AppSystemSettings),
  ITEM_LINK("Encoder",         AppEncoderMenu),
  ITEM_LINK("Wi-Fi",           AppWifi),
  ITEM_LINK("Mixer",           AppMixerSettings),
  ITEM_LINK("Speaker",         AppSpotifySettings),
  ITEM_LINK("NeoPixel",        AppNeoPixel),
  ITEM_LINK("Battery+Sleep",   AppBattery),
  ITEM_LINK("About",           AppAbout),
};
static CfgPage PageSettings = { "Settings", SETTINGS_ITEMS, 8, 0, false, nullptr };
static void setEnter() { cfgOpen(&PageSettings); }

static const App AppSettings = {
  "Settings", iconGear, setEnter, nullptr, nullptr, cfgDraw, cfgButton, cfgKnob,
  nullptr, nullptr
};

/* ###########################################################################
 * #                       MIXER CONTROL APP                                 #
 * ######################################################################### */

extern const App AppMixer;      // referenced by its own launch gate below

static void iconMixer(int x, int y, int s) {
  // Glyph width is odd (21) inside an odd box, so padding splits evenly.
  const int capW = 5, sp = 8, gw = 2 * sp + capW;
  int left = x + (s - gw) / 2;
  int top = y + 5, bot = y + s - 5;
  const int capY[3] = { 12, 6, 16 };
  for (int i = 0; i < 3; i++) {
    int c0 = left + i * sp;
    display.drawFastVLine(c0 + capW / 2, top, bot - top, SH110X_WHITE);
    display.fillRect(c0, y + capY[i], capW, 3, SH110X_WHITE);
  }
}

static float    mxLastSent = -1.0f;
static uint32_t mxLastSentMs = 0, mxLastUserMove = 0;
static bool     mxUserActive = false;
static uint32_t mxSeenSeq = 0;
static int      mxSeenGroup = -1, mxSeenNum = -1;

static void mxEnter() {
  knobResetSteps();
  mlSetMeters(true);
  if (mlUdpUp) mlRequestStrip();
  mxLastSent = mlFader;
  mxUserActive = false;
  mxSeenSeq = mlFaderSeq;
  mxSeenGroup = mlGroup;
  mxSeenNum = mlNum;
}

static void mxExit() {
  mlSetMeters(false);
  if (mlDemo) mlDemoEnd();      // never leave the app in a simulated state
}

/* A console change is just a new value.
 *
 * The servo needed an ownership arbiter, echo windows and confirmation
 * counts because a physical shaft had to be reconciled with a remote value.
 * A relative encoder has no position to reconcile: overwrite mlFader and the
 * next detent continues from there. This function is what is left.        */
static void mxOnFader(float f) {
  /* While the user is turning, an inbound value is our own send coming back
   * via /xremote, delayed by the round trip. Applying it overwrites a fader
   * the user has since moved further, so the reading snaps backwards for a
   * frame -- rubber-banding that gets worse the faster you spin. Record it
   * as the last known console value, but do not apply it. */
  if (mxUserActive) { mxLastSent = f; return; }
  mlFader = f;
  mxLastSent = f;
}

/* Ring output for the mixer. Fader mode shows the same value as the on-screen
 * bar; meter mode mirrors the level display, zoned like a console meter. */
static void mxRing() {
  if (!npEnable || npMxMode == 0) return;
  npFrameStart();
  if (npMxMode == 2) {
    if (mlMeterValid)
      npScale(mlMeter, 0, true, false, 0,
              npMxClip != 0, npMxClip == 2);
  } else {
    bool useB = (mxCfgEncBtn == 1 && btnIsDown(B_ENC));
    npScale(mlFader, useB ? npMxRbCol : npMxFadCol, false,
            npMxEnds != 0, npMxWarnCol, false, false);
  }
  npFrameEnd();
}

static void mxTick() {
  /* Changing strip must drop user-active, otherwise the echo guard above
   * would reject the new strip's fader value and the display would keep
   * showing the previous channel's level. */
  if (mlGroup != mxSeenGroup || mlNum != mxSeenNum) {
    mxSeenGroup = mlGroup;
    mxSeenNum = mlNum;
    mxUserActive = false;
    mxLastSent = mlFader;
  }
  if (mlFaderSeq != mxSeenSeq) { mxSeenSeq = mlFaderSeq; mxOnFader(mlFader); }

  int st = knobSteps();
  bool held = btnIsDown(B_ENC);

  /* Shaft button held + turn = change strip, so the fader and the channel
   * share one control without a mode. */
  if (mxCfgEncBtn == 0 && held) {
    if (st) mlStripStep(st > 0 ? +1 : -1);
    return;
  }

  if (st) {
    bool useB = (mxCfgEncBtn == 1 && held);
    float step = (float)(useB ? mxCfgRateB : mxCfgRateA) / 1000.0f;
    // Acceleration is opt-in per rate: useful on a coarse rate, unwanted on
    // a fine one, and which is which is now the user's choice.
    bool accel = useB ? (mxCfgAccelOn == 2 || mxCfgAccelOn == 3)
                      : (mxCfgAccelOn == 1 || mxCfgAccelOn == 3);
    if (accel) st *= knobAccelMul();
    mlFader = constrain(mlFader + st * step, 0.0f, 1.0f);
    mxUserActive = true;
    mxLastUserMove = millis();
    if (elapsed(mxLastSentMs, SEND_MIN_MS)) {
      mlSendFader(mlFader);
      mxLastSent = mlFader;
      mxLastSentMs = millis();
    }
  } else if (mxUserActive && elapsed(mxLastUserMove, (uint32_t)mxCfgUserHold)) {
    // Make sure the console ends up with the exact final value, since sends
    // during a fast spin are throttled and the last one may have been eaten.
    if (fabsf(mlFader - mxLastSent) > 0.0005f) {
      mlSendFader(mlFader);
      mxLastSent = mlFader;
      mxLastSentMs = millis();
    }
    mxUserActive = false;
  }
  mxRing();
}

static void mxButton(BtnId b, BtnEv e) {
  if (b == B_C) mlStripStep(+1);
  else if (b == B_A) mlStripStep(-1);
  else if (b == B_B && e == EV_PRESS) mlGroupStep();
  // Only acts in Click mode; in Hold+Turn mode the shaft button is a
  // modifier handled in mxTick, so a bare click deliberately does nothing.
  else if (b == B_ENC && e == EV_PRESS && mxCfgEncBtn == 2) mlGroupStep();
}

static void mxDraw() {
  char label[12], hdr[26];
  mlStripLabel(label, sizeof(label));
  if (mlName[0]) snprintf(hdr, sizeof(hdr), "%s %s", label, mlName);
  else           snprintf(hdr, sizeof(hdr), "%s", label);

  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  // Battery takes the top-right corner; the link indicator moves to the
  // footer, where there is room. At y=10 it would have collided with the
  // size-2 dB readout below.
  printClipped(0, 0, hdr, batShowMixer ? CONTENT_W - 38 : CONTENT_W);
  if (batShowMixer) drawBatteryStatus(CONTENT_W, 0, 1);

  float db = faderToDb(mlFader);
  char dbs[12];
  if (db <= -190.0f) snprintf(dbs, sizeof(dbs), "-oo");
  else snprintf(dbs, sizeof(dbs), "%+.1f", db);
  display.setTextSize(2);
  display.setCursor(0, 11);
  display.print(dbs);
  display.setTextSize(1);
  display.print("dB");

  display.setCursor(0, 30);
  display.print("F");
  drawBar(10, 29, CONTENT_W - 10, 7, mlFader);
  display.drawFastVLine(11 + (int)(0.75f * (CONTENT_W - 12)), 27, 2, SH110X_WHITE);

  display.setCursor(0, 42);
  display.print("S");
  if (mlMeterValid) {
    /* Rises are taken instantly and only the fall is smoothed, the way a
     * hardware meter behaves. Smoothing both ways would hide transients and
     * add apparent latency; smoothing the release alone just removes the
     * flicker between console updates without ever making a peak late. */
    static float mxMeterDisp = 0.0f;
    static uint32_t mxMeterMs = 0;
    float k = (float)constrain((int)mxCfgMeterSm, 0, 95) / 100.0f;
    if (mlMeter >= mxMeterDisp) mxMeterDisp = mlMeter;
    else if (elapsed(mxMeterMs, 20)) {
      mxMeterMs = millis();
      mxMeterDisp += (mlMeter - mxMeterDisp) * (1.0f - k);
    }
    float mdb = (mxMeterDisp > 1e-7f) ? 20.0f * log10f(mxMeterDisp) : -200.0f;
    drawBar(10, 40, CONTENT_W - 10, 11,
            (mdb - METER_FLOOR_DB) / (0.0f - METER_FLOOR_DB));
    if (elapsed(mlPeakMs, PEAK_HOLD_MS)) mlPeak *= 0.90f;
    float pdb = (mlPeak > 1e-7f) ? 20.0f * log10f(mlPeak) : -200.0f;
    float pf = constrain((pdb - METER_FLOOR_DB) / (0.0f - METER_FLOOR_DB), 0.0f, 1.0f);
    int px = 11 + (int)(pf * (CONTENT_W - 12));
    if (px > 11) display.drawFastVLine(min(px, CONTENT_W - 2), 41, 9, SH110X_WHITE);

    /* Ticks at the colour-zone boundaries, so the OLED meter and the ring
     * agree about where amber and red begin. One per boundary above zone 1,
     * which is two marks for three zones and three for four. */
    int zn = constrain((int)npMxZones, 2, 4);
    int th[3] = { (int)npMxT2, (int)npMxT3, (int)npMxT4 };
    for (int k = 0; k < zn - 1 && k < 3; k++) {
      int t = constrain(th[k], 1, 99);
      if (k > 0) t = max(t, constrain(th[k - 1], 1, 99));
      display.drawFastVLine(11 + (int)(t / 100.0f * (CONTENT_W - 12)), 37, 3,
                            SH110X_WHITE);
    }
  } else {
    display.setCursor(10, 42);
    display.print("(no meter)");
  }

  display.setCursor(0, 54);
  bool encHeld = btnIsDown(B_ENC);
  if (mxCfgEncBtn == 0 && encHeld)      display.print("hold: strip");
  else if (mxCfgEncBtn == 1 && encHeld)
    display.printf("B %ld.%ld%%", (long)(mxCfgRateB / 10), (long)(mxCfgRateB % 10));
  else display.printf("%.0f%%", mlFader * 100.0f);
  /* USER/IDLE removed: it only reported whether the knob had moved recently,
   * which the fader reading already shows. The link state is the part worth
   * keeping, and at y=58 its descenders were clipped by the panel edge. */
  const char *lnk = mlDemo ? "DEMO" : !wifiOnline() ? "Wi-Fi offline"
                  : (mlLinkOk() ? "OK" : "??");
  display.setCursor(CONTENT_W - (int)strlen(lnk) * 6, 54);
  display.print(lnk);

  drawButtonLabels(G_UP, G_RIGHT, G_DOWN);
}

// Entering the mixer requires Wi-Fi and a console IP. Missing prerequisites
// divert into the relevant setup screen, which resumes here once satisfied.
static void mixerIpThenRun() { navReplace(&AppMixer); }

// Replaces the IP screen rather than stacking on it, so backing out of the
// demo returns to wherever the user came from.
static void mixerDemoStart() {
  mlDemoBegin();
  if (cur() == &AppIpInput) navReplace(&AppMixer);
  else navPush(&AppMixer);
}

static void mixerLaunch() {
  if (mlDemo) { navPush(&AppMixer); return; }
  if (!wifiConfigured() || !wifiOnline()) {
    gAfterConnect = &AppMixer;
    gWifiGateway = true;
    navPush(&AppWifi);
    return;
  }
  if (!mixerIpSet()) {
    ipInputBegin(&mxCfgIp, "Mixer IP", mixerIpThenRun);
    gIpAllowDemo = true;
    gIpDemo = mixerDemoStart;
    navPush(&AppIpInput);
    return;
  }
  navPush(&AppMixer);
}

const App AppMixer = {
  "Mixer Control", iconMixer, mxEnter, mxExit, mxTick, mxDraw, mxButton, nullptr,
  mixerLaunch, nullptr
};

/* ###########################################################################
 * #                            HOME                                         #
 * ######################################################################### */

static const App *HOME_APPS[] = { &AppMixer, &AppSpeaker, &AppSettings };
static const int  HOME_N = sizeof(HOME_APPS) / sizeof(HOME_APPS[0]);
static int homeSel = 0;

static void homeEnter() { knobInputMode(); }
static void homeMove(int d) { homeSel = (homeSel + d + HOME_N) % HOME_N; }

static void homeButton(BtnId b, BtnEv e) {
  if (b == B_C) homeMove(+1);
  else if (b == B_A) homeMove(-1);
  else if (btnSelect(b) && e == EV_PRESS) launchApp(HOME_APPS[homeSel]);
}

static void homeKnob(int s) { if (s) homeMove(s > 0 ? +1 : -1); }

static void homeDraw() {
  const int ic = 27, gap = 9;   // odd size => integer centre, even padding
  int total = HOME_N * ic + (HOME_N - 1) * gap;
  int x0 = (CONTENT_W - total) / 2, y = 14;   // room for the battery row
  for (int i = 0; i < HOME_N; i++) {
    int x = x0 + i * (ic + gap);
    display.drawRoundRect(x, y, ic, ic, 7, SH110X_WHITE);
    if (i == homeSel) {
      display.drawRoundRect(x - 1, y - 1, ic + 2, ic + 2, 8, SH110X_WHITE);
      display.drawRoundRect(x - 2, y - 2, ic + 4, ic + 4, 9, SH110X_WHITE);
    }
    if (HOME_APPS[i]->drawIcon) HOME_APPS[i]->drawIcon(x, y, ic);
  }
  drawCentered(51, HOME_APPS[homeSel]->name);
  // Battery and text drop 1px; drawBatteryStatus puts the bolt one row
  // higher than the body, so the bolt stays flush with the top edge.
  drawBatteryStatus(CONTENT_W, 1, 1);
  drawButtonLabels(G_RIGHT, G_SELECT, G_LEFT);
}

// Back at the top of the stack has nowhere else to go, so it offers sleep.
static bool homeBack() { navPush(&AppSleepMenu); return true; }

static const App AppHome = {
  "Home", nullptr, homeEnter, nullptr, nullptr, homeDraw, homeButton, homeKnob,
  nullptr, homeBack
};

/* ###########################################################################
 * #                          SETUP / LOOP                                   #
 * ######################################################################### */

static uint32_t gLastDraw = 0;

static void registerApps() {
  const App *list[] = {
    &AppHome, &AppMixer, &AppSettings, &AppSystemSettings, &AppEncoderMenu,
    &AppEncoderTest, &AppEncoderConfig, &AppWifi, &AppWifiScan, &AppWifiInfo,
    &AppWifiConnect, &AppMixerSettings, &AppSpotifySettings, &AppTextInput,
    &AppIpInput, &AppDropdown, &AppSpeaker, &AppSonyScan, &AppSpeakerInfo,
    &AppSpotifySetup, &AppSpotifyMenu, &AppPlayback, &AppAbout,
    &AppNeoPixel, &AppNpGlobal, &AppNpMixer, &AppNpSpeaker, &AppColourEdit,
    &AppBattery, &AppSleepMenu, &AppBattWarn, &AppChargeWait, &AppNpTest,
  };
  ALL_APPS_N = sizeof(list) / sizeof(list[0]);
  for (int i = 0; i < ALL_APPS_N; i++) ALL_APPS[i] = list[i];
}

static bool navRestorable(const App *a) {
  return a && a != &AppTextInput && a != &AppIpInput &&
         a != &AppDropdown && a != &AppWifiConnect && a != &AppWifiScan &&
         a != &AppSonyScan;
}

static void navRestore() {
  gStack[0] = &AppHome;
  gDepth = 0;
  if (sysStartup != 0) {
    int32_t s[4] = { navSaved0, navSaved1, navSaved2, navSaved3 };
    int maxDepth = (sysStartup == 1) ? 1 : 3;
    for (int i = 1; i <= maxDepth; i++) {
      if (s[i] < 0 || s[i] >= ALL_APPS_N) break;
      const App *a = ALL_APPS[s[i]];
      if (!navRestorable(a)) break;
      gStack[++gDepth] = a;
    }
  }
  if (cur() && cur()->onEnter) cur()->onEnter();
}

void setup() {
  /* First thing, before the radio or any TLS: move mbedTLS's large
   * allocations off the internal heap. See tlsHeapInit() for why the internal
   * heap was the whole "connect -1" story. */
  tlsHeapInit();
  Serial.begin(115200);
  /* On the S2's native USB CDC a write blocks until the host drains it, up
   * to 100ms each. With no monitor attached that stalls whichever task is
   * printing -- netTask included. Never wait. */
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);
#endif

  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);
  pinMode(PIN_BTN_C, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  encBegin();
  /* Still NOT enabling extmem malloc globally. It never changed the symptom,
   * and for a good reason now understood: mbedTLS does not use the general
   * allocator, so redirecting it was aimed at the wrong pool entirely.
   * tlsHeapInit() above redirects mbedTLS's own allocator instead, which is
   * both narrower and the one that matters. */

  /* Prefer PSRAM for the big response buffer; fall back to internal RAM if
   * the board is built without PSRAM enabled. */
  syBody = (char *)ps_malloc(SY_BODY_CAP);
  if (!syBody) syBody = (char *)malloc(SY_BODY_CAP);
  if (syBody) syBody[0] = 0;
  /* Prove it rather than assume it: a large malloc should now land in
   * external RAM. gExtMalloc drives the on-screen readout, so this is
   * checkable without a serial cable. */
  void *probe = malloc(40000);
  gExtMalloc = probe && esp_ptr_external_ram(probe);
  if (probe) free(probe);
  Serial.printf("[boot] psram=%u psblk=%u free=%u maxblk=%u extmalloc=%d "
                "tlsps=%d\n",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getMaxAllocPsram(),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                (int)gExtMalloc, (int)gTlsPsram);

  npBegin();
  pinMode(PIN_VCHG, INPUT);
  analogReadResolution(12);

  nvLoad();

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(OLED_ADDR, true)) Serial.println("SH1107 begin() FAILED");
  applySystemCfg();
  display.setTextWrap(false);
  display.clearDisplay();
  display.display();

  netMutex = xSemaphoreCreateRecursiveMutex();
  syMutex  = xSemaphoreCreateRecursiveMutex();
  xTaskCreate(netTask, "net", 6144, nullptr, 1, nullptr);
  xTaskCreate(syTask,  "sy", 16384, nullptr, 1, nullptr);   // TLS is stack hungry

  registerApps();
  /* Woken by the charger after shutting down flat: stay in a minimal mode
   * with the radio off until there is enough charge to be useful. */
  if (rtcWokeEmpty && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    batChargeWait = true;
    gStack[0] = &AppChargeWait;
    gDepth = 0;
    batNoteActivity();
    return;                       // skip Wi-Fi entirely while charging
  }

  /* Waking from DEEP sleep is a fresh boot, so sleepResume() -- which is
   * where the wake guard is normally armed -- never runs. The button that
   * woke the board is still held at this point, so without arming it here the
   * wake press was delivered as a real press, and if it stayed down it
   * repeated. That is the bag scenario: sustained pressure walking into a
   * settings page and changing values. */
  esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
  if (wc == ESP_SLEEP_WAKEUP_EXT0 || wc == ESP_SLEEP_WAKEUP_EXT1 ||
      wc == ESP_SLEEP_WAKEUP_GPIO)
    gWakeGuard = true;

  if (rtcGoHome) { rtcGoHome = 0; navSaved1 = navSaved2 = navSaved3 = -1; }
  navRestore();
  batNoteActivity();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // modem sleep adds 100ms+ of jitter
  if (wifiConfigured()) { WiFi.begin(wifiSsid, wifiPass); wifiTune(); }
}

void loop() {
  knobUpdate();
  mlTick();
  /* Skip polling entirely while the wake press is still held. Previously the
   * guard was evaluated and then ignored, so buttonsPoll() saw the button
   * down and raised a press anyway -- and with a repeat delay set, holding it
   * a moment longer produced repeats too. */
  if (!wakeGuardBlocking()) buttonsPoll();

  if (cur() && cur()->onKnob) {
    int s = knobSteps();
    if (s) { cur()->onKnob(s); batNoteActivity(); }
  }
  batSupervise();

  npTouched = false;
  if (cur() && cur()->tick) cur()->tick();
  // Nothing claimed the ring this pass, so it belongs to no screen: dark.
  if (cur() != &AppNpTest && (!npTouched || !npEnable)) npClear();

  if (gNavDirty) { gNavDirty = false; navSaveNow(); }
  nvService();

  if (elapsed(gLastDraw, DISPLAY_MS)) {
    gLastDraw = millis();
    gFrame++;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    CONTENT_W = CONTENT_W_NARROW;      // wide screens opt in from their draw()
    if (cur() && cur()->draw) cur()->draw();
    display.display();
  }
}


/* ===========================================================================
 *                              C H A N G E L O G
 *
 *  Major version = a new mini-app or a new subsystem.
 *  Minor version = tweaks, bug fixes, UI work.
 *  Reconstructed from this sketch's development history.
 *
 *  --- 1.x  mini-app framework ---------------------------------------------
 *  v1.0   Nav stack, App structs with null-checked handlers, config-page
 *         engine shared by menus and settings, Home screen with icons, Mixer
 *         Control (X32/M32 OSC over UDP), Servo Test. A+B back gesture with
 *         45ms deferred dispatch. Servo detach-when-idle with settle
 *         re-baselining.
 *  v1.1   Compile fix: hoisted Glyph / BtnId / BtnEv / App / MenuItem above
 *         the first function, since the .ino preprocessor injects prototypes
 *         there referencing types not yet declared. Pot calibration set to the
 *         measured 1545 / 80 mV endpoints.
 *  v1.2   Chevrons reduced to 7x5, selection ring thickened. Replaced the
 *         useless FREE toggle (identical to the resting state) with HOLD,
 *         which energises the servo so back-drive force can be compared.
 *
 *  --- 2.x  settings system ------------------------------------------------
 *  v2.0   NVS persistence with debounced writes, System Settings, Servo
 *         configuration, Wi-Fi submenu (scan / manual / info with saved
 *         networks), Mixer settings, text entry, IP entry with validation,
 *         enum dropdowns, scrollbars, prerequisite launch gates.
 *  v2.1   Text entry reworked: knob mapped absolutely across the character set
 *         rather than by relative detents (nine sweeps to cross ~93 chars),
 *         with arming on cursor move so existing characters survive. Added
 *         click-into-character editing and an OK button. Same for IP entry.
 *
 *  --- 3.x  Speaker Control mini-app ---------------------------------------
 *  v3.0   Sony Audio Control API over the LAN, SSDP discovery of ScalarWebAPI
 *         devices, speaker settings page, third home icon.
 *  v3.1   Settings entry relabelled Spotify -> Speaker.
 *  v3.2   Volume latency: persistent TCP, coalesced writes instead of one
 *         request per intermediate value, polling suppressed while turning
 *         (a stale poll was read as a remote change and drove the servo
 *         backwards). Added the Vol Min / Vol Max window.
 *  v3.3   Centre button switched to local mute, after the probe showed the
 *         soundbar never registers the Spotify Connect stream with avContent.
 *         Speaker Info became a scrollable text buffer.
 *
 *  --- 4.x  Spotify Web API ------------------------------------------------
 *  v4.0   Built-in OAuth config page served over the LAN, refresh-token
 *         storage, metadata polling, transport control, now-playing layout.
 *         Volume deliberately left on the local Sony API to stay instant.
 *  v4.1   Token exchange fixed: HTTP Basic credentials as documented,
 *         URL-encoded code, tolerant paste handling, full error surfaced.
 *  v4.2   Reclaimed the right margin (5px -> 2px), widened content. Merged
 *         with a branch adding uiWide() for label-less screens.
 *  v4.3   Column geometry corrected -- divider and glyphs had moved by
 *         different amounts so the line read as drifting left. Icon boxes
 *         28 -> 27px: an even box has no integer centre and every symbol
 *         leaned one pixel left.
 *  v4.4   Spotify mark baked in as a 27x27 bitmap; drawn at runtime it was
 *         either a WiFi fan (sweep too steep) or stepped at the apex.
 *  v4.5   Volume number made to track the knob directly. Remote changes now
 *         need the same value on two consecutive polls, removing the
 *         jump-up-then-back glitch. Volume UI moved to the bottom half.
 *  v4.6   market=from_token to shrink the player response, exponential
 *         back-off on failed polls, no network calls while the knob moves.
 *  v4.7   Network work moved to a dedicated FreeRTOS task -- blocking HTTP in
 *         loop() had been freezing knob, display and buttons together. Mutex
 *         for the shared sockets, queued button presses, and the parser
 *         stopped calling substring() on a multi-kilobyte response.
 *  v4.8   Fixed a real deadlock: syRequest held the mutex while its 401 retry
 *         re-entered via the token refresh, and a plain FreeRTOS mutex is not
 *         recursive. Added a sync gate so entering the app no longer wrote the
 *         knob's resting position as the volume. Heap guard before TLS;
 *         netTask stack 12K -> 16K.
 *  v4.9   Spotify pretty-prints its JSON ("key" : value), so every parser
 *         matching "key": silently found nothing. Added whitespace-tolerant
 *         scanners. Response streams into a static buffer instead of a heap
 *         String, which had driven free heap down to ~13KB.
 *  v4.10  Version stamp and changelog.
 *
 *  --- 5.x  scrubbing and About --------------------------------------------
 *  v5.0   Press-and-hold cue/review scrubbing with configurable hold time,
 *         speed, granularity and acceleration, all under a new Playback page.
 *         Elapsed and remaining/total clocks under the progress bar, both
 *         interpolated locally. Play/pause now freezes and resumes the local
 *         progress clock immediately rather than waiting for Spotify to
 *         confirm. About screen with a KnobOS logo. Version scheme changed to
 *         major.minor and earlier entries renumbered.
 *  v5.1   Clawd mascot replaced with the real artwork, transposed from
 *         column-major to the row-major order drawBitmap wants, and the About
 *         footer re-laid out for a horizontal rather than vertical sprite.
 *  v5.2   Volume mapping anchored rather than absolute. The servo cannot park
 *         to better than a step or two, so quantising raw knob position
 *         invented a user change the moment a sync or a remote adoption
 *         finished -- the jump on entry and the snap-back after adjusting at
 *         the soundbar were one bug, not two. Anchoring makes the delta zero
 *         at every re-anchor point, with re-anchoring at the stops to avoid
 *         wind-up, plus an 800ms write cooldown after adopting a remote
 *         change.
 *         The back chord no longer fires its own members: suppressed buttons
 *         are excluded from release handling, and only chord members wait out
 *         the new Back Win delay while other buttons fire immediately.
 *         Both progress clocks derive from one elapsed-second value so they
 *         tick together. Marquee driven by the frame counter instead of
 *         millis(), which had been beating against the 50ms frame period.
 *         "..." shown while a Spotify command is in flight. Divider moved to
 *         x=112. About logo enlarged to size 3 with letter spacing, dial
 *         centred on the letterform rather than the character cell.
 *         Fixed mixed-type max()/min() calls that fail to compile where
 *         uint32_t is unsigned long and size_t is unsigned.
 *  v5.3   Fixed the Spotify "..." indicator latching on permanently. It had
 *         been added by a mechanical text replacement that landed inside
 *         unbraced single-statement if bodies, so the flag escaped its
 *         condition and was set unconditionally every tick. Now set through a
 *         helper with explicit braces, only when Spotify is the transport
 *         backend, and with a 6s expiry so a missed clear cannot latch it
 *         again.
 *  v5.4   Reverted v5.2's anchored mapping, which fixed the phantom write by
 *         making volume relative to wherever the knob happened to be -- so
 *         the scale drifted and the ends stopped reaching Vol Min / Vol Max.
 *         Volume is absolute again; the parking error that made absolute
 *         mapping misfire is now attacked at the source. The servo closes the
 *         loop on its own pot: after settling it measures the error and
 *         nudges again, up to four corrections, and only reports settled once
 *         it is within tolerance. Post-move state is taken from the measured
 *         position rather than the commanded one. Remote changes are adopted
 *         only when they differ by two or more steps and at most every 1.2s,
 *         which stops the knob hunting while the soundbar's own volume is
 *         being adjusted.
 *  v5.5   Fixed the knob jumping nonstop on entry. On settle the app was
 *         adopting the knob's *measured* position as spaSent, so a park that
 *         fell short left spaSent disagreeing with spkVol; the remote-change
 *         detector then "corrected" it, parked short again, and looped
 *         forever. spaResync now records the speaker's value, and a motion
 *         gate stops the residual mismatch being written back -- the mapping
 *         stays absolute, so full travel is still min..max. Park tolerance
 *         and retry count exposed as Servo settings, since 1.5% of travel was
 *         tighter than the gearbox can repeat and the loop never converged.
 *         Verified with a real arduino-cli build for esp32:esp32:um_feathers2.
 *  v5.6   Mixer Demo mode. The IP entry screen gained Demo and Clear buttons
 *         reached by moving the cursor past the last digit, and Settings >
 *         Mixer gained Clear IP and Demo Mode rows. Demo drives the real
 *         Mixer UI -- same strips, servo sync, meters and de-oscillation --
 *         from a local console model, intercepted at udpTx, mlTick and
 *         mlLinkOk so no UI code needed demo branches. Nothing is sent on the
 *         wire and the flag is deliberately not persisted.
 *  v5.7   Speaker Control no longer requires a soundbar. The two halves are
 *         independent -- volume is the Sony box on the LAN, metadata and
 *         transport are Spotify's cloud -- so with no speaker configured the
 *         app runs as a pure Spotify remote. The launch gate now demands a
 *         speaker only when Spotify cannot carry the app alone, the sync gate
 *         no longer waits for a volume reading that will never arrive, the
 *         knob and volume writes are disabled rather than left half-live, and
 *         the lower half of the screen says so instead of showing a dead 0-50
 *         bar. Centre button falls back to play/pause since mute needs
 *         hardware.
 *  v5.8   Mixer console-to-knob sync no longer runs away. It still had the
 *         bug the speaker app shed in v5.5: after a park it took the knob's
 *         measured position as mxLastSent, which always disagreed with the
 *         console because the park lands short, so the next /xremote push
 *         looked like a fresh remote change and it parked again, forever.
 *         mxAdopt now records the console value, a motion gate stops the
 *         residual mismatch being read as input, remote changes need two
 *         matching reports, and a 1.2s cooldown follows each adopted move.
 *         Added Settings > Mixer > Follow to disable console-to-knob driving
 *         entirely while leaving the display live.
 *  v5.9   Servo moves no longer abort mid-swing. svGoFader sized its hold
 *         time from the last *commanded* angle, which after any manual
 *         turning (servo limp) says nothing about where the shaft physically
 *         is -- so when the stale angle sat near the target the hold came out
 *         near zero and the servo detached mid-swing, coasting to an
 *         arbitrary spot. Travel is now estimated from the measured knob
 *         position, and the closed-loop trims scale their hold with the size
 *         of the correction instead of a fixed 110ms. This is why
 *         console-to-knob looked erratic while the speaker app seemed fine:
 *         mixer parks always follow heavy manual turning, speaker parks
 *         mostly follow servo moves where the commanded angle was fresh.
 *
 *  --- 6.x  direction arbiter ---------------------------------------------
 *  v6.0   Mixer sync rebuilt around an explicit owner (MX_IDLE / MX_KNOB /
 *         MX_MIXER) instead of letting both directions run at once behind
 *         echo windows, deadbands and confirmation counts. Those guards
 *         always overlapped somewhere: after a park the shaft relaxes a
 *         degree or two, the knob path read that as user input and sent it
 *         back, and the console moved the *wrong way*. Now only one side owns
 *         the fader at a time -- while the console drives, knob input is not
 *         merely filtered but ignored, and ownership is held through a 300ms
 *         relaxation window after the servo settles. Console reports arriving
 *         mid-move retarget the servo instead of queueing a stale
 *         destination, and claiming MX_MIXER requires the console value to
 *         have actually changed, which is what stops a short park being
 *         re-triggered by /xremote repeats. Removed mxAdopt, mxBaseline,
 *         mxPendingMove and the two-report counter, all now redundant.
 *         The pot readout is replaced by "Knob -> Mixer" / "Knob <- Mixer",
 *         showing which side currently owns control.
 *  v6.1   Servo positioning rebuilt as a closed loop that keeps the servo
 *         ATTACHED throughout. The old scheme guessed a travel time,
 *         detached, let the knob coast, waited a fixed settle window,
 *         measured, and allowed one trim -- every step of which could go
 *         wrong, and each detach let an analog servo relax before the next
 *         measurement. The pot already reports position, so the guessing was
 *         never needed: command the feed-forward angle, hold it energised,
 *         wait until the shaft is genuinely STATIONARY, measure the residual
 *         and nudge, repeat until inside tolerance, and only then detach.
 *         New SV_SEEK state; SV_HOLD is now only used by the servo test page.
 *         Corrections are damped (Park Gain, default 60%). At full gain a
 *         system whose real angle-to-fader gain is about twice the calibrated
 *         one -- which 2:1 external gearing produces -- overshoots every
 *         correction and rings at constant amplitude forever, never reaching
 *         target. That is the "moves erratically, never arrives" symptom.
 *         Under-correcting always converges; Park Trys raised to 4 to pay
 *         for the extra step or two.
 *
 *  --- 7.x  rotary encoder --------------------------------------------------
 *  v7.0   Servo and feedback pot replaced by a rotary encoder with a push
 *         switch. This is not a swap of one input device for another: the
 *         entire class of bugs that dominated 5.x and 6.x came from having a
 *         physical absolute position that had to be reconciled with a remote
 *         value. A relative encoder has no position to reconcile -- a remote
 *         change simply overwrites the value and the next detent continues
 *         from there -- so the servo state machine, the ownership arbiter,
 *         park/adopt/anchor, motion gates, echo windows, two-poll
 *         confirmations and Follow all deleted outright. The mixer's sync
 *         logic went from roughly 90 lines to about 30, and the speaker's
 *         from about 80 to 25.
 *
 *         Input: interrupt-driven quadrature decode on both edges of both
 *         channels through a 16-entry state table. Illegal transitions map
 *         to zero, so contact bounce is rejected by the decoder rather than
 *         by a debounce delay and no detent is missed however fast the knob
 *         is spun. Optional acceleration for continuous values (fader,
 *         volume, character pick); menus deliberately do not use it, since
 *         skipping rows feels broken.
 *
 *         Text and IP entry now step relatively and wrap, replacing the
 *         absolute mapping with its arming and hysteresis workarounds.
 *
 *         Shaft button is a fourth button (B_ENC), never a member of the
 *         back chord. Its action is chosen per app, mutually exclusive:
 *         Speaker = Play/Pause (default) | Mute | Next | Off, Mixer =
 *         Hold+Turn strip (default) | Click group | Off, Text = Caps Lock
 *         (default) | Confirm | Off. Mute is bound to a two-second hold
 *         rather than a click so it cannot silence the room by accident.
 *
 *         Settings: Servo menu and Servo Test replaced by Encoder menu and a
 *         simpler Encoder Test showing detent count, direction, live
 *         acceleration multiplier, clicks and hold timing. Every servo and
 *         pot setting removed; new Per Detent, Reversed, Accel, Accel Win,
 *         Fader/detent and Hold. Mixer loses Deadband, Min Move, Echo Tol,
 *         Echo Win and Follow, which existed only to police the servo.
 *
 *         Pins: encoder A/B reuse the freed servo and pot pins (10, 7), the
 *         switch is on 11; all three use internal pull-ups, so no external
 *         parts. ESP32Servo dependency dropped.
 *  v7.1   Shaft click now also acts as select on every generic UI screen --
 *         menus, settings, dropdowns, Wi-Fi scan and info, speaker scan, Home
 *         and IP entry -- via a btnSelect() helper. Apps that give the shaft
 *         button its own meaning (Speaker, Mixer, Text entry, Encoder Test)
 *         handle B_ENC themselves and are untouched.
 *         Also repaired two things lost in a branch merge: the IP screen's
 *         Demo and Clear buttons existed only as declarations, so they were
 *         unreachable and undrawn, and the text-entry caps indicator had been
 *         rendering on the IP screen instead of the text screen.
 *  v7.2   Text entry gained a Clear button beside OK, reached the same way --
 *         keep pressing right past the last character. Clears every field
 *         that uses the editor, SSID and password included. The shaft click
 *         follows the selection when it sits on either button, so the encoder
 *         alone can drive the whole screen.
 *  v7.3   Caps lock is now a real mode. It had only remapped the current
 *         character to the matching letter in the other half of TI_CHARS, so
 *         the next rotation stepped straight back into the other case. The
 *         rotation order is filtered instead: only letters of the active case
 *         are offered, so the lock holds for everything typed afterwards, and
 *         toggling converts the character under the cursor in place.
 *         Wi-Fi scan no longer hangs on "Scanning...". scanComplete() returns
 *         -2 on failure and the old code accepted only n >= 0, so a failed
 *         scan never resolved -- and a scan started while the radio is busy
 *         associating fails routinely, which is why it worked in one place
 *         and not another. Failures and stalls now retry three times, then
 *         report it with a retry action. Hidden SSIDs are included too.
 *         spkActive and syActive marked volatile: netTask reads them while
 *         the UI task writes them, so caching either was a real hazard.
 *         "No track info" split from "Contacting Spotify...", since one
 *         message covering both a completed empty poll and no poll at all
 *         made a silent polling failure look like an idle queue.
 *  v7.4   Serial.setTxTimeoutMs(0) when built with USB CDC on boot. On the
 *         S2's native USB a write blocks until the host drains it, up to
 *         100ms each, so with no monitor attached the printing task stalls --
 *         netTask included.
 *         Speaker Info now also reports the Spotify side: configured, authed,
 *         wifi, active, poll count, last HTTP code, body size, track found,
 *         fail streak, free heap and the last error. The device has to be
 *         able to explain itself on its own screen, because serial is
 *         unavailable unless USB CDC On Boot is enabled in the board menu.
 *         Added Settings > Speaker > Spotify > Poll Now to force a metadata
 *         fetch, and spaEnter queues the first poll directly rather than
 *         relying on syService() to schedule it.
 *  v7.5   Metadata polls were being blocked by my own heap guard. A live TLS
 *         session holds roughly 33KB of record buffers open between requests
 *         because of setReuse, so free heap settles near 12KB after the first
 *         call -- below the 45KB floor. Transport commands slipped through in
 *         the moments heap happened to be recovered, which is why play/pause
 *         worked while nothing ever polled. The floor is now conditional:
 *         45KB when a handshake is needed, 12KB when reusing a live session,
 *         since the response body is a static buffer and needs almost no
 *         heap. Added syTlsIdle(): the session is dropped after 20s idle
 *         in-app, or 3s after leaving, returning the memory.
 *         404 on a transport call is Spotify's NO_ACTIVE_DEVICE, not a
 *         failure -- reported as "no active device" like a 204. Error bodies
 *         from 4xx replies are now captured and their message shown, instead
 *         of a bare status code.
 *  v7.6   Wi-Fi scan reported "radio busy" almost instantly. scanComplete()
 *         returns -2 both for a failed scan and for no scan running, which is
 *         what a rejected start looks like, and the retry fired the moment it
 *         saw -2 -- so all three attempts were spent inside a few hundred
 *         milliseconds, before the radio could possibly free up. Retries are
 *         now spaced 1.2s apart, up to seven, with the attempt counter shown.
 *         WiFi.mode() is only called when the mode actually needs changing,
 *         since it resets the radio.
 *         Wi-Fi info: a saved network's detail view gained a Connect button,
 *         so a remembered network can be rejoined without retyping its
 *         password.
 *         Encoder Accel range starts at 1 rather than 0. Both 0 and 1
 *         rendered as "Off", so the first press appeared to do nothing.
 *  v7.7   Mixer review before live use, two real bugs found:
 *         mxOnFader applied every inbound value even while the user was
 *         turning. Those are our own sends coming back through /xremote a
 *         round trip later, so they overwrote a fader the user had since
 *         moved further -- the reading snapped backwards for a frame, worse
 *         the faster the spin. Inbound values are now recorded but not
 *         applied while the user is active.
 *         That guard alone would have broken strip changes, since a new
 *         strip's fader would have been rejected as an echo, so a group/num
 *         change now clears user-active first.
 *         Added Hold = Fine: holding the shaft button divides the step by
 *         Fine Div (default 5) and suppresses acceleration, since
 *         multiplying the step would defeat the purpose. Enc Btn is now
 *         Hold+Turn Ch | Hold = Fine | Click Group | Off.
 *  v7.8   Mixer fader step generalised from one rate plus a fine divisor to
 *         two independent rates. Rate A is the default and Rate B applies
 *         while the shaft button is held, with nothing requiring B to be the
 *         smaller of the two -- setting B above A turns the hold into a
 *         coarse/fast mode instead of a fine one. Acceleration is opt-in per
 *         rate (Accel On: Off | Rate A | Rate B | Both), since it helps on a
 *         coarse rate and ruins a fine one, and which is which is now the
 *         user's choice rather than an assumption. Fader/det moved off the
 *         Encoder page, where it was a mixer-only setting in a global menu.
 *  v7.9   Track info failing to appear when the speaker is unreachable was a
 *         starvation bug, not a Spotify one. netTask picked work from a
 *         fixed-priority chain with the volume poll at the top; an
 *         unreachable speaker costs a full connect timeout per attempt, and
 *         spkService re-queued one every 500ms, so netTask spent nearly all
 *         its time failing to reach a box that was not there and the Spotify
 *         poll below it never ran. Transport still worked because commands
 *         are queued on demand and sit above the polls. Two fixes: the polls
 *         are now selected round-robin from a rotating cursor, so none can be
 *         crowded out however slow its neighbours are; and speaker polling
 *         backs off up to 16x once the speaker stops answering, resetting on
 *         the first success. Commands still take priority over all polls.
 *
 *  --- 8.x  NeoPixel ring --------------------------------------------------
 *  v8.0   12-pixel NeoPixel ring on GPIO 7, pixel 0 at 6 o'clock running
 *         clockwise, driven as a gauge. Brightness is applied per pixel in
 *         software rather than through setBrightness(), because the clip
 *         indicator has to be able to ignore the master level entirely -- a
 *         clip you might miss is not a clip indicator.
 *         Smoothing lights the pixel straddling the level proportionally to
 *         how far the level reaches into it, so the ring reads as continuous
 *         instead of twelve discrete steps. Same idea as anti-aliasing a
 *         line; switchable off.
 *         Meter mode colours each pixel by ITS OWN position on the scale,
 *         not by the current level, which is what makes it read like a
 *         console meter rather than a bar that changes colour. Two to four
 *         zones with adjustable thresholds; default green to 65%, amber to
 *         88%, red above. Clip LED: Off | No Dim | Full Bright.
 *         Level modes can mark the extremes -- first pixel at 0%, last at
 *         100% -- in a separate colour, and only at the extremes, so the ring
 *         is not permanently complaining. The zero case is drawn explicitly
 *         since at zero the scale itself lights nothing.
 *         New colour editor: RGB and HSV side by side as two views of one
 *         value, each rewriting the other, with the active column marked.
 *         A/C step the six fields, the knob adjusts, holding the shaft button
 *         switches coarse to fine, and B cycles the live ring preview between
 *         all pixels, just the affected ones, and off.
 *         Settings > NeoPixel holds the on/off switch with Global, Mixer and
 *         Speaker submenus. Origin and Reverse are there too, so the ring can
 *         be mounted any way round without rewiring.
 *  v8.1   Battery and sleep.
 *         The plain FeatherS2 has NO on-board battery divider -- the Neo has
 *         one on IO2 and the S3 on GPIO2, but the original board's own FAQ
 *         says there is no built-in way to read VBAT. So VBAT sensing expects
 *         an external divider on GPIO 3, with the ratio and both endpoints
 *         configurable. 5V presence is read from the divider on GPIO 10, and
 *         drives the charging bolt on the battery glyph.
 *         Sleep: on the ESP32-S2 only GPIO0..21 are RTC-capable, so buttons B
 *         (38) and C (33) physically cannot wake the chip from deep sleep.
 *         Light sleep has no such limit, so it is the default -- all four
 *         buttons wake it and the screen resumes exactly where it was, at a
 *         few hundred microamps instead of tens. Deep sleep is offered for
 *         storage and wakes on the shaft button. Either way the wake press is
 *         swallowed: input is ignored until every button is released, so the
 *         press that wakes the device never also activates what is under it.
 *         Empty-battery shutdown wakes on the CHARGER line rather than on a
 *         timer. Polling to re-check the level would spend energy from an
 *         already flat pack and could not change the outcome -- nothing but a
 *         charger raises the charge -- whereas an RTC pin costs nothing while
 *         asleep and fires exactly when something can be done. On charger
 *         wake the board runs radio-off in a charge-wait screen until the
 *         Resume At threshold, then restarts cleanly rather than bringing
 *         half-initialised subsystems up from a low-power state.
 *         Warnings latch at 20%% and 10%% and rearm on charge. Home and the
 *         mixer show icon plus percentage; back on Home opens a sleep menu
 *         with a larger readout.
 *         Also fixed: ALL_APPS was still sized 24 while the list had grown to
 *         32 entries, which was overrunning the array.
 *  v8.2   Sleep menu is now Sleep / Off, with the bottom button as back and
 *         a back glyph on the label column. Off always uses deep sleep
 *         whatever the default mode is -- it is the "put it away" option, so
 *         the lowest draw is what is wanted -- and wakes on the shaft button.
 *         NeoPixel review before first hardware test, two real faults:
 *         show() was being called on every loop pass, several hundred times a
 *         second, which achieves nothing visible and keeps the RMT peripheral
 *         busy for ~360us each time. It now pushes only when the buffer has
 *         actually changed, and never faster than the display refresh.
 *         Meter zone thresholds could be edited out of order, and with t3
 *         below t2 the zones swapped over; they are ordered at use so a bad
 *         setting degrades to a sensible meter instead of a scrambled one.
 *         The mixer battery readout moved to the true top-right: at y=10 it
 *         overlapped the size-2 dB figure. The link indicator moved to the
 *         footer, which had room.
 *  v8.3   Hold the NeoPixel data line low across sleep. Clearing the ring
 *         sends zeros, but once the CPU sleeps GPIO7 stops being driven, and
 *         a floating WS2812 data input can latch noise into the pixels. On a
 *         ring powered straight from BAT that would keep draining the pack
 *         through a sleep that is supposed to cost microamps. The pad is now
 *         driven low and latched with gpio_hold_en/gpio_deep_sleep_hold_en,
 *         released on wake and at boot.
 *  v8.4   Spotify broke when the NeoPixel and battery code landed, and the
 *         cause was my own heap guard. The 32KB-vs-45KB floor for a fresh TLS
 *         handshake was measured when there was room to spare; the new
 *         globals and the RMT driver pushed idle heap just below it, so every
 *         handshake was refused before it started. The speaker kept working
 *         because it is plain HTTP with no guard at all. Worse, syPoll then
 *         overwrote the specific "low heap" reason with a generic "no
 *         connection", which hid exactly the information needed. Floor
 *         lowered to what a handshake actually needs, and a specific error is
 *         never replaced by the generic one.
 *         "Speaker no response" while the speaker plainly worked was the
 *         v7.9 poll backoff outrunning a fixed 4s link window; the window now
 *         scales with the poll interval.
 *         Occasional lost volume detents: a failed write was dropped, and the
 *         next adopt pulled the speaker's unchanged value back into the
 *         target, silently undoing the turn. Failed writes are now requeued
 *         twice.
 *         Sleep menu redrawn inside the content area -- the buttons ran under
 *         the label column and the hint line fell off the bottom edge.
 *         Mixer footer moved up 2px and the USER/IDLE tag dropped; it only
 *         reported whether the knob had moved recently, which the fader
 *         reading already shows.
 *         Battery reading gained a rolling average (Avg Window, 20s) plus a
 *         separate display latch (Update Ivl, 5s). The average smooths ADC
 *         noise; the latch stops the number twitching between two adjacent
 *         percentages when the average drifts across a boundary.
 *  v8.5   Charging bolt moved to the left of the battery glyph. Inside a
 *         5px-tall body it was not legible; the layout widens by the bolt
 *         only while charging, so nothing shifts otherwise.
 *         Mixer reclaimed the row the USER/IDLE tag was using: the signal bar
 *         is now 11px instead of 9 and the rows are spread out. Zone-boundary
 *         ticks are drawn above it, one per boundary above zone 1, using the
 *         same thresholds as the ring so the OLED meter and the NeoPixel
 *         agree about where amber and red begin. Fader percentage and link
 *         state now share the bottom row.
 *  v8.6   Both Spotify AND the speaker failing at once pointed at heap, not
 *         at either service. Two causes: the guard measured TOTAL free heap
 *         when a TLS handshake needs one large CONTIGUOUS block -- a
 *         fragmented heap reports plenty free while the biggest usable piece
 *         is far too small -- so it now checks getMaxAllocHeap(). And the
 *         kept-alive TLS session was pinning roughly 30KB for 20s at a time,
 *         which on this board is enough to starve the plain-HTTP speaker
 *         requests too. That hold is now 2.5s in-app and immediate on exit,
 *         which is what makes the speaker snap-back and the Spotify -1 stop
 *         being the same bug wearing two faces.
 *         Button auto-repeat can be switched off: Rpt Delay accepts 0.
 *         Sleep menu: percentage first, battery to its right, bolt on the
 *         far left, hint text flush left so it clears the label column.
 *  v8.7   Spotify's "connect failed (-1)" was at least partly a timeout, not
 *         a name-resolution fault. v7.5 cut the TLS connect window to 2.5s,
 *         but an ECDHE handshake plus certificate work takes well over a
 *         second on this chip and DNS can add another, so a slow-but-healthy
 *         connection was being abandoned and surfaced as the same -1 a real
 *         DNS failure gives. Raised to 9s for the token exchange and 8s for
 *         API calls; it all runs in netTask, so waiting costs the UI nothing.
 *         The token path also had no heap check at all, so a memory shortage
 *         there could only ever appear as a connection error. It now reports
 *         the largest free block, and the connect error carries that figure
 *         too, so the next failure says which of the two it is.
 *         Sleep menu layout applied (percentage first, battery right, bolt
 *         far left, hint flush left) -- these were written in v8.6 but the
 *         edit did not take.
 *  v8.8   "connect -1 blk 34804" settled it: the request passed the guard,
 *         attempted the handshake, and the allocation inside mbedTLS failed.
 *         It wants a larger CONTIGUOUS block than 34.8KB -- its record
 *         buffers are 16KB each by default -- so no amount of timeout or
 *         retry work could have helped. The fix is to give the internal heap
 *         more room: PSRAM must be enabled in the board menu, and the
 *         response buffer now allocates from PSRAM when present, returning
 *         4.6KB of internal SRAM (static RAM fell from 89,620 to 85,012
 *         bytes). Boot logs psram/free/maxblk, and Speaker info shows PSRAM
 *         size and largest block, since without those the cause is invisible.
 *  v8.9   Spotify displaying but not responding was the 2.5s TLS hold from
 *         v8.6. That was a workaround for a heap shortage PSRAM has now
 *         removed, and it forced a fresh handshake on EVERY poll -- seconds
 *         each, on a serial netTask -- so transport commands queued behind
 *         one and appeared to do nothing. The session is now held for 120s
 *         when PSRAM is present, and commands are checked before polls rather
 *         than sharing the round-robin with them.
 *         Added Settings > NeoPixel > Test: writes the ring directly, with no
 *         reference to npEnable, the per-app modes, the rate limiter or the
 *         dirty check. That separates a driver or wiring fault from app logic
 *         not calling the ring, which cannot be told apart from the outside.
 *         It also reports the library's pixel buffer pointer -- a null there
 *         means its allocation failed and nothing will ever light, which
 *         otherwise looks exactly like bad wiring.
 *
 *  --- 9.x  power and polish ----------------------------------------------
 *  v9.0   Deep sleep woke the instant it was entered. Two causes, both
 *         needed: the ordinary GPIO pull-up is switched off when the digital
 *         domain powers down, so the shaft pin floated and read LOW -- which
 *         IS the ext0 wake condition -- and the button that selected "Off"
 *         was still held when sleep began. The RTC pull-up is now enabled and
 *         the release is waited for. The charge-sense line gets an RTC
 *         pull-down for the same reason in reverse.
 *         Light sleep still acted on the wake press because the guard was
 *         evaluated and then ignored; buttonsPoll() is now skipped while it
 *         is held, so no press and no repeats.
 *         Mixer meter gained release-only smoothing (Meter Smooth, 55%).
 *         Rises are taken instantly and only the fall is damped, the way a
 *         hardware meter behaves -- smoothing both directions would hide
 *         transients and make peaks late, which is the opposite of what a
 *         meter is for.
 *         Battery glyph widened by one pixel so the first bar no longer sits
 *         against the border and read as part of it; the bolt is a pixel
 *         tighter to pay for it. Sleep screen uses fixed columns so nothing
 *         shifts when the charger comes and goes, with the voltage centred.
 *         Home icons down 2px, label down 1px, battery down 1px with the bolt
 *         still flush to the top edge. Battery readout on the mixer is now
 *         optional, since it eats into long channel names.
 *  v9.1   Speaker AND Spotify dead while the mixer kept working was the tell:
 *         the mixer talks UDP straight from the UI task, while both of the
 *         others go through netTask behind a SINGLE shared mutex. A Spotify
 *         handshake or a stalled connect holds that lock for seconds, so the
 *         speaker's polls were timing out on the lock itself -- reporting "no
 *         response" about a speaker that was answering fine. They use
 *         separate client objects and never needed serialising against each
 *         other, so the lock is now split in two, with a short 2.5s wait for
 *         the LAN side and a long one for TLS.
 *         Also: a kept-alive TLS session that the far end has since closed
 *         fails on send rather than on connect and is indistinguishable from
 *         having no network. Such a failure now drops the session and retries
 *         once with a fresh handshake, and the idle hold dropped from 120s to
 *         45s, below the ~60s most servers allow.
 *         Sleep screen matches the home readout (bolt, battery, percentage),
 *         centred as a block with every slot reserved. Bolt 1px left.
 *  v9.2   "blk 34xxx" WITH PSRAM enabled was the missing piece. Enabling it in
 *         the board menu makes the external RAM visible but does not put it
 *         in the general malloc pool -- the Arduino build reserves it for
 *         explicit ps_malloc. So 8MB sat unused while the largest allocatable
 *         block stayed near 34KB, and mbedTLS still could not get the ~40KB
 *         it needs. heap_caps_malloc_extmem_enable(8192) lowers the threshold
 *         above which malloc is served externally, so TLS record buffers go
 *         to PSRAM while small allocations stay internal. Boot probes a 40KB
 *         malloc and checks whether it really landed in external RAM; the
 *         answer appears on the NeoPixel Test and Speaker info pages, so it
 *         can be verified without a serial cable. The heap guards defer to
 *         that probe, since getMaxAllocHeap reports the internal pool only
 *         and would otherwise keep refusing requests that can now succeed.
 *  v9.3   With "ext y" confirming PSRAM is in the malloc pool, memory is no
 *         longer the constraint and the remaining -1 is a real connection
 *         failure -- consistent with the -80 dBm link. WiFi modem power save
 *         is now disabled and TX power set to maximum at every connect.
 *         Power save parks the radio between beacons, which costs little on
 *         a strong link but on a weak one becomes dropped packets and long
 *         retransmits; a TLS handshake is a chain of small round trips, so it
 *         fails where a single tiny LAN request still succeeds. That
 *         asymmetry is exactly the "speaker fine, Spotify not" pattern.
 *         TLS timeouts raised to 15s/12s and a failed connect now always
 *         retries once after dropping any half-open session, not only when a
 *         kept-alive session was suspected.
 *         The display no longer flashes "Spotify idle" during a request:
 *         syErr is cleared on entry, so the idle text appeared for the whole
 *         time a request was in flight and read as a wrong answer rather than
 *         a pending one. Requests are tracked and show "Contacting Spotify".
 *         Speaker info now reports RSSI, since signal strength has become a
 *         suspect worth being able to see.
 *  v9.4   "Spotify idle - no device" was mostly an artefact of my own error
 *         handling. syErr was cleared at the start of every request and again
 *         whenever a token refresh succeeded, so the last real failure was
 *         erased before it could be read, and the screen fell through to the
 *         idle text in the gap -- which, with the failure backoff stretching
 *         to 32s, was most of the time. The error now survives until a poll
 *         actually succeeds, the backoff is capped at 2x, and a 200 with no
 *         track distinguishes "playing nothing" from an unparsed item, since
 *         from the outside those look the same. The artist row, unused when
 *         there is no track, now carries the last HTTP code, body length and
 *         RSSI so the evidence is on the main screen rather than buried.
 *  v9.5   The speaker breaking again was my own regression. netTask is a
 *         single task, so while it sits in a Spotify connect it cannot
 *         service a queued volume write -- and v9.3 raised the timeouts to
 *         15s AND retried every failure, which could tie it up for most of a
 *         minute. Timeouts back to 5s/6s, the retry restricted to a reused
 *         session that may have been closed remotely, and a circuit breaker
 *         added: after four consecutive failures Spotify stands down for a
 *         minute, so a broken cloud link can never take the local controls
 *         with it.
 *         For the -1 itself: the speaker is reached by IP and Spotify by
 *         name, so resolution is the one step only Spotify depends on. The
 *         host is now resolved explicitly before the token request, and a
 *         failure there reports as DNS rather than folding into the same -1
 *         that a refused or timed-out connection produces.
 *  v9.6   Spotify moved onto its own task, which resolves a tension I had
 *         been trading back and forth for six versions without noticing it
 *         was a false choice. A TLS handshake genuinely needs several seconds
 *         on this chip, but netTask is serial, so any timeout long enough for
 *         Spotify also stalled the speaker's volume writes behind it.
 *         Shortening the timeout fixed the speaker and broke Spotify;
 *         lengthening it did the reverse -- which is exactly the oscillation
 *         v9.3 and v9.5 show. They are different workloads, one LAN-fast and
 *         one cloud-slow, and needed separate tasks rather than a compromise
 *         timeout suiting neither. Each already had its own mutex.
 *         netTask now handles only the Sony side with a small stack; syTask
 *         owns Spotify polling, transport and the TLS session lifetime, with
 *         the 16KB stack a handshake needs. Command queues split to match, so
 *         the two cannot interleave. With the tasks independent the Spotify
 *         timeouts return to 12s/10s, which is what a real handshake needs
 *         and what v8.9 had when metadata last worked.
 *  v9.7   Reverted the two things added after v8.9, the last build where
 *         Spotify metadata worked, that could plausibly break a handshake.
 *         extmem malloc (v9.2) never changed the symptom -- it failed
 *         identically before and after -- and redirecting every allocation
 *         above 8KB into PSRAM is risky: the Wi-Fi driver allocates buffers
 *         on demand that must be DMA-reachable, and DMA cannot reach external
 *         RAM. ESP-IDF defaults that threshold to 16KB for that reason.
 *         Maximum TX power (v9.3) raises the current spike during transmit,
 *         and on battery a sag mid-handshake is indistinguishable from a
 *         refused connection. Power-save-off is kept; it is a real win on a
 *         marginal link and carries no such risk.
 *         The internal headroom extmem was meant to buy arrived for free in
 *         v9.6 anyway: moving Spotify to its own task let netTask's stack
 *         drop from 16KB to 6KB, returning about 10KB -- close to what the
 *         NeoPixel driver took when this first started failing. syBody still
 *         uses ps_malloc, which is contained rather than a global redirect.
 *  v9.8   Waking from DEEP sleep still delivered the wake press, because deep
 *         sleep resumes through a fresh boot and sleepResume() -- where the
 *         wake guard is armed -- never runs. The boot path now checks the
 *         wakeup cause and arms it, so the still-held button is swallowed the
 *         same way light sleep already handled it.
 *         Added Stuck Cut (System Settings, default 10s): a button held
 *         longer than that is treated as pressure rather than intent and
 *         ignored entirely until released, suppressing both its press and any
 *         repeats. A device pressed against something in a bag can no longer
 *         repeat its way through a settings page. Auto-repeat can also still
 *         be switched off outright with Rpt Delay = 0.
 *  v9.9   "connect -1 blk 34xxx" is solved, and every PSRAM measure since
 *         v8.8 was aimed at the wrong heap.
 *         The Arduino core ships CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y, which
 *         pins EVERY mbedTLS allocation to internal RAM. mbedTLS therefore
 *         never touches the general allocator, so ps_malloc for syBody (v8.8)
 *         and heap_caps_malloc_extmem_enable (v9.2) could not reach it -- and
 *         that is exactly why v9.2 "failed identically before and after". The
 *         8MB was real, visible, and unreachable by the one caller that
 *         needed it.
 *         What it needs is large. CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN is 16384
 *         with ASYMMETRIC_CONTENT_LEN off, so a handshake allocates BOTH
 *         record buffers at full size: ~16.5KB each, ~33KB together, before
 *         the certificate chain. ESP.getMaxAllocHeap() reports the largest
 *         free INTERNAL block -- the very pool in question -- and it reads
 *         32-38KB here. So "blk 34804" was never a comfortable margin above a
 *         32KB guard; it was a hair under what was actually being asked for.
 *         The first buffer fits, the second only just, the chain parse does
 *         not. A figure sitting on the boundary is also why this was
 *         intermittent for so long, why it "worked at v8.9", and why every
 *         timeout, retry and task change appeared to move it: they each
 *         shifted idle heap by a kilobyte or two across that line.
 *         The fix is mbedtls_platform_set_calloc_free at boot, sending
 *         mbedTLS allocations of 1KB and up to PSRAM with an internal
 *         fallback. That is what CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC does at
 *         build time; a stock Arduino install cannot edit sdkconfig, so it is
 *         done at runtime. It is safe where the v9.7 revert was not: mbedTLS
 *         record buffers are filled by lwIP copies, never by DMA, and the
 *         Wi-Fi driver's DMA buffers do not come through mbedTLS at all.
 *         Diagnosis, so the next failure cannot be ambiguous either. HTTP -1
 *         flattens a refused socket, a timeout and an out-of-memory handshake
 *         into one number, which is what made this unfalsifiable for six
 *         versions. NetworkClientSecure keeps mbedTLS's own error across the
 *         stop() that follows a failed connect, so it is now read back and
 *         shown: "tls -0x7F00" is SSL_ALLOC_FAILED, "no socket" means TLS was
 *         never reached. Speaker info gained tlspool (which heap mbedTLS is
 *         using), psblk, and the decoded error string.
 *         Two bookkeeping faults found alongside. syRequestOnce set
 *         syTlsLive unconditionally, so after a failed connect the guard
 *         believed a session was up and let the next request through on a
 *         third of the memory a handshake needs; it now reflects the result.
 *         And syTokenRequest stopped the TLS client without clearing
 *         syTlsLive, leaving the same lie behind after every token refresh.
 *         The reuse-retry now samples the flag before the call, since
 *         checking it afterwards would no longer ever see a reuse.
 *         The heap floors were rewritten to describe the pool they actually
 *         guard: they now apply only on a board with no PSRAM.
 * ========================================================================= */
