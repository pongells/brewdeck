#pragma once
// Brewdeck core API, version 1. Brewdeck's own UI (src/ui) and the example (src/ui_minimal) run on it; the guide is
// docs/custom-ui.md.
//
// The firmware in three layers:
//   board  panel, touch, backlight, pins, frame buffer (one per hardware: the Sunton ESP32-S3 RGB panel, the Guition
//          ESP32-P4 MIPI-DSI panel, the emulator and the web demo); hidden behind bd_display_* / bd_touch_*
//   core   Wi-Fi, finding the machine, REST + WebSocket, protobuf, settings storage, logging, and the ONLY place
//          that sends anything to the machine (it enforces when a command is allowed)
//   UI     screens and interaction: anything that implements the four bd_ui_* functions at the end of this file,
//          with any graphics library (LVGL 9 through the small adapter bd_lvgl9.h, or none)
//
// Rules for a UI:
//   - Everything runs on one task, the UI task ("core", started by main.cpp). The core calls bd_ui_tick() once per pass and
//     bd_ui_event() for each event; the UI calls bd_* functions only from inside those (or from LVGL callbacks,
//     which run on the same task). No locking needed on the UI side.
//   - Pointers the core returns are read-only and valid until the next bd_ui_tick() / until bd_ui_event() returns.
//     Copy what you want to keep.
//   - Commands never block. They return BD_OK when queued, or why they were refused (offline, busy, out of range, ...);
//     the machine's answer comes later as a BD_EV_COMMAND event: OK once the machine reports the change, NOT_TAKEN
//     when it did not within a few seconds.
//   - The UI cannot send anything else to the machine: no pump, valve, heater, steam, descale or delete. Those are
//     not in this API, by design.
//   - The core has no graphics library. It gives the UI a landscape screen of bd_display()->width x height pixels
//     (RGB565): the UI draws into its own buffers and hands finished rectangles to bd_display_flush(); touches come
//     from bd_touch_read(). The board turns that into whatever the panel needs (frame buffer, rotation, DMA).
//   - At boot the core clears the screen to black and starts the UI; it draws nothing of its own. Brewdeck is not made
//     by or affiliated with the Gaggiuino project, and neither is a UI built on this core: please say so in yours
//     (Brewdeck's splash and Menu > About do).
//   - Notices: the core filters the noise (repeats of the update notice, the "Fill the water tank!" notice the machine repeats about once a second) and hands each one to
//     the UI as an event; keeping a list of them is the UI's choice.
//
// Compatibility: a UI is always compiled together with the core, so what is kept is the source: a UI written for
// version 1 keeps compiling and working with every later 1.x core. Within version 1 nothing is removed or renamed and
// nothing changes meaning; new functions, events, enum values and struct fields may be added (handle unknown event
// types and enum values by ignoring them). A change that breaks that is version 2. A UI names the version it was
// written for in BD_UI; the core logs it, and warns when the UI wants a newer one than it has.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BD_API_VERSION 1          // 1 = the first stable API

// command limits (bd_set_stop_weight, bd_set_brew_temp); outside them a command is refused with BD_ERR_RANGE
#define BD_STOP_WEIGHT_MAX 200    // g (0 = no stop weight)
#define BD_BREW_TEMP_MIN 1        // degC (0 = heater off: never sent)
#define BD_BREW_TEMP_MAX 105

// ---- results ------------------------------------------------------------------------------------------------------
typedef enum {
  BD_OK = 0,                // queued (the machine's answer: BD_EV_COMMAND)
  BD_ERR_OFFLINE,           // no link to the machine (or, for WebSocket commands, not commands_ready yet)
  BD_ERR_BUSY,              // the machine is not idle in HOME (shot, steam, hot water, flush, another mode)
  BD_ERR_RANGE,             // value outside the allowed range (brew temp 1-105 degC, stop weight 0-200 g)
  BD_ERR_PROFILE_CHANGED,   // the active profile is no longer the one named in the request
  BD_ERR_NO_SCALE,          // tare without a connected scale
  BD_ERR_PENDING,           // the same request is still waiting for the machine (a profile pick)
  BD_ERR_UNKNOWN_ID,        // no such profile / stored shot
} bd_result;

// ---- the machine right now ----------------------------------------------------------------------------------------
typedef enum { BD_LINK_CONNECTING = 0, BD_LINK_NO_MACHINE, BD_LINK_ONLINE } bd_link_state;
// why the link is down: Wi-Fi network not in range / password refused (only before the first join since boot) /
// other; machine name unknown / no reply
typedef enum { BD_WHY_NONE = 0, BD_WHY_WIFI_NO_AP, BD_WHY_WIFI_AUTH, BD_WHY_WIFI_OTHER, BD_WHY_NAME_UNKNOWN,
               BD_WHY_NO_REPLY } bd_why;
// the machine's readiness as its web UI names it; BD_RD_UNKNOWN when it does not report one
typedef enum { BD_RD_UNKNOWN = -1, BD_RD_WAITING = 0, BD_RD_WARMING, BD_RD_HEATING, BD_RD_READY, BD_RD_STANDBY,
               BD_RD_STEAMING, BD_RD_FLUSHING, BD_RD_MAINTENANCE } bd_readiness;
typedef enum { BD_SCALE_UNKNOWN = 0, BD_SCALE_BT, BD_SCALE_WIRED, BD_SCALE_PREDICTIVE } bd_scale_type;

typedef struct {
  bd_link_state link;
  bd_why        why;              // when link != BD_LINK_ONLINE
  // live values (WebSocket ~3/s, REST ~1/s as the fallback); only meaningful while online
  float temperature;              // boiler, degC
  float pressure;                 // bar
  float weight;                   // scale, g (the last reading stays after the scale is gone: see scale_present)
  int   water_level;              // percent
  int   warmup_remaining;         // s
  bd_readiness readiness;
  bool  brewing, steaming, hot_water;
  char  mode[20];                 // HOME, BREW_AUTO, STEAM, FLUSH, FLUSH_AUTO, DESCALE, ... (commands: idle in HOME)
  float target_temp;              // the boiler's current target, degC (REST targetTemperature)
  bool  scale_present;
  bd_scale_type scale_type;
  int   steam_set;                // steam set point, degC (0 = not known yet)
  int   profile_id;               // the active profile (status), 0 = not known yet
  char  profile_name[48];
  // the active profile as the machine runs it (d_act_prof): its values may differ from the stored profile after a
  // change made on a screen, and right after a switch it can already name the new profile before the status does
  struct { char name[48]; float stop_weight, brew_temp; } running;   // 0 = none / not known
  // the machine can take commands now (stop weight, brew temp, flush, tare): its WebSocket session is up and it has
  // reported its active profile. False for a few seconds up to ~30 s after the machine comes back online; profile
  // picks need only the link. Show those controls as unavailable while false.
  bool  commands_ready;
  bool  update_available;         // the machine reported a firmware update (install it in its web UI)
  char  update[32];               // its release name ("" = not given)
  // versions (Menu > About); "" = not known
  char  core_version[16], core_board[28], screen_version[28], web_version[28];
} bd_machine;

// ---- profiles -----------------------------------------------------------------------------------------------------
#define BD_MAX_PROFILES 24
typedef struct {
  int   id;
  char  name[48];                 // as the machine reports it
  bool  active;
  bool  detail;                   // phases / stop_weight / brew_temp below are filled in
  int   phases;
  float stop_weight, brew_temp;
  uint32_t last_used;             // newest stored shot of this profile in the history (Unix time), 0 = none / not loaded
} bd_profile;
typedef struct { int count; bd_profile p[BD_MAX_PROFILES]; } bd_profiles;

typedef enum { BD_PHASE_PRESSURE = 0, BD_PHASE_FLOW } bd_phase_type;
typedef enum { BD_CURVE_INSTANT = 0, BD_CURVE_LINEAR, BD_CURVE_EASE_IN, BD_CURVE_EASE_OUT, BD_CURVE_EASE_IN_OUT } bd_curve;
#define BD_MAX_PHASES 24
typedef struct {
  uint8_t  type, curve;           // bd_phase_type, bd_curve
  bool     has_start;             // false: starts where the previous phase ended
  float    start, end;            // bar or ml/s
  uint32_t ramp_ms;
  float    limit;                 // flow limit of a pressure phase / pressure limit of a flow phase (0 = none)
  uint32_t stop_ms;               // stop conditions, 0 = not set
  float    stop_p_above, stop_p_below, stop_water, stop_weight;
} bd_phase;
typedef struct { int id, n; bd_phase ph[BD_MAX_PHASES]; float stop_weight, brew_temp; } bd_profile_shape;

// A profile's preview: its phases as polylines for a w x h box (SCHEMATIC: phases mostly end on water / weight /
// pressure conditions, so their lengths are estimates). Time runs over the box width (estimated total in secs); one
// vertical scale for bar and ml/s, 0 at y = h - 3 up to 10 at y = 5 (values above 10.5 are clipped). Four series:
// what each phase drives (the target) and what it only limits, per quantity; each is nseg polylines,
// segment i = pts[seg[i] .. seg[i+1]).
#define BD_PREVIEW_PTS 200
typedef struct { int16_t x, y; } bd_point;
typedef struct { int n, nseg; int seg[BD_MAX_PHASES + 1]; bd_point pts[BD_PREVIEW_PTS]; } bd_preview_series;
enum { BD_PV_PRESSURE = 0, BD_PV_PRESSURE_LIMIT, BD_PV_FLOW, BD_PV_FLOW_LIMIT, BD_PV_SERIES };
typedef struct { float secs; bd_preview_series s[BD_PV_SERIES]; } bd_preview;   // ~3.6 KB

// ---- shots --------------------------------------------------------------------------------------------------------
// one point, x10 integers as the machine sends them: time 0.1 s, bar, ml/s, g, degC, target pressure / flow; phase
typedef struct { uint16_t t, p, f, w, temp, tp, tf; uint8_t ph; } bd_shot_point;
#define BD_SHOT_MAX 720
typedef struct {
  int      id;                    // machine's shot id (0 = the live shot, not stored yet)
  int      dur10;                 // duration, 0.1 s
  uint32_t ts;                    // when it was pulled (Unix time; small = the machine's clock was not set)
  char     profile[48];
  int      n;
  bd_shot_point pt[BD_SHOT_MAX];  // ~11.5 KB, in PSRAM
} bd_shot;
#define BD_HISTORY_MAX 50
// a stored shot: machine id, duration (0.1 s), when (Unix time), profile name (printable ASCII, cut at 39 chars)
typedef struct { uint32_t id, dur10, ts; char profile[40]; } bd_history_entry;
typedef struct { int n; bd_history_entry e[BD_HISTORY_MAX]; } bd_history;   // newest first

// ---- the screen's own network -------------------------------------------------------------------------------------
#define BD_MAX_NETS 16
typedef struct { char ssid[33]; int8_t rssi; bool locked; } bd_wifi_net;
typedef struct {
  char   ssid[33];                // joined network, "" = none
  char   target[33];              // network it is trying to join, "" = none set up
  int8_t rssi;
  int    channel;
  char   ap[18];                  // access point (BSSID)
  char   screen_ip[16];           // "" without Wi-Fi
  char   machine_addr[64];        // configured name or IP
  char   machine_where[40];       // the machine's IP once found, else the name being looked up
  char   why_ip[16];              // with BD_WHY_*: the address that did not answer
} bd_network;

// ---- core and display ---------------------------------------------------------------------------------------------
typedef enum { BD_RESET_POWER_ON = 0, BD_RESET_PIN, BD_RESET_SOFTWARE, BD_RESET_CRASH, BD_RESET_WATCHDOG,
               BD_RESET_BROWNOUT, BD_RESET_DEEP_SLEEP, BD_RESET_USB, BD_RESET_OTHER } bd_reset;
typedef struct {
  int      api_version;           // BD_API_VERSION of the core
  char     build[24];             // core build date
  uint32_t uptime_s;
  bd_reset reset;                 // why the screen last started (OTHER: e.g. the system watchdog, which ends every
                                  // flash on the ESP32-P4; WATCHDOG: our code hung)
  uint32_t free_internal, free_psram;   // bytes
  char     version[40];           // the firmware's version: its release tag ("v0.2.0"), "v0.2.0-3-gabc1234" between
                                  // releases, "" when not known (emulator)
} bd_core_info;

// ---- reading state (UI task only; valid until the next bd_ui_tick) -------------------------------------------------
const bd_machine       *bd_machine_state(void);
const bd_network       *bd_network_state(void);
const bd_profiles      *bd_profile_list(void);        // empty until the machine answered
const bd_profile_shape *bd_active_shape(void);        // id 0 until loaded
bool                    bd_profile_preview(const bd_profile_shape *s, int w, int h, bd_preview *out);   // false = no phases
// (Brewdeck's review also draws the planned rest of a shot after it stopped, from src/core/preview.h: not in the API
// yet; it may be added in a later 1.x)
const bd_history       *bd_shot_history(void);        // the machine's newest 50 stored shots
const bd_shot          *bd_live_shot(void);           // the running (or last) shot as recorded on this screen
const bd_core_info     *bd_core(void);
bool                    bd_local_time(int *h, int *m, int *wday);   // false until NTP set the clock
// change counters: compare with the last value seen to redraw only what changed
uint32_t bd_seq_machine(void), bd_seq_profiles(void), bd_seq_shape(void), bd_seq_history(void), bd_seq_network(void);

// ---- commands to the machine (all: only online and idle in HOME, checked again just before sending) ----------------
// save: also store the change on the machine (the stored profile, and the boot profile for a select), 3 s after the
// last change; false = only for now (the machine keeps it until another profile is picked or it restarts)
bd_result bd_select_profile(int id, bool save);                   // POST /api/profile-select/<id>; one at a time
bd_result bd_set_stop_weight(int profile_id, float grams, bool save);   // the ACTIVE profile only (else PROFILE_CHANGED); 0-200 g, 0 = none
bd_result bd_set_brew_temp(int profile_id, int deg, bool save);   // 1-105 degC (0 = heater off: refused); same rules
typedef enum { BD_FLUSH_OFF = 0, BD_FLUSH_ON, BD_FLUSH_TIMED } bd_flush;   // ON: the brew switch flushes; TIMED: the machine's own timed flush
bd_result bd_flush_mode(bd_flush mode);                           // OFF is also accepted from a flush mode
bd_result bd_tare(void);                                          // needs a connected scale; OK once it reads ~0
// reading stored shots (GETs): always queued, sent once the machine is online and idle; answers as BD_EV_SHOT_LOADED
bd_result bd_load_previous_shot(const char *profile, int dur10);  // the stored shot of this profile before the one just made
bd_result bd_load_stored_shot(uint32_t id);                       // a shot from the history, plus the one before it (same profile)

// ---- the screen itself --------------------------------------------------------------------------------------------
void bd_wifi_scan(void);                                          // answer: BD_EV_WIFI_SCAN
void bd_wifi_join(const char *ssid, const char *pass);            // answer: BD_EV_WIFI_JOIN; saved on success; password never logged
void bd_wifi_forget(void);
void bd_wifi_retry(void);
void bd_set_machine_address(const char *host);                    // name (mDNS, ".local" optional) or IP; saved, used at once
void bd_set_backlight(int pct);   // 0 = off, 20-100 = the panel's dimmest useful .. fully on (the board maps it)
// firmware releases (BD_HAS_RELEASES): the core looks for a newer release at start and once a day (only its signed
// description: version, size, checksum); installing downloads it into the free app slot, checks it and restarts into
// it (progress: BD_EV_UPDATE, as for any update). Only while no shot, steam, hot water or flush runs (else BUSY);
// UNKNOWN_ID = no newer release known. A download stops if one starts.
typedef enum { BD_RELEASE_UNKNOWN = 0, BD_RELEASE_CHECKING, BD_RELEASE_CURRENT, BD_RELEASE_AVAILABLE,
               BD_RELEASE_FAILED } bd_release_state;
typedef struct {
  bd_release_state state;         // FAILED: the last check got no valid answer (the previous version, if any, stays)
  char     version[16];           // the newest release ("v0.3.0"), "" until a check answered
  uint32_t size;                  // its image, bytes
  bool     installing;            // bd_release_install accepted, download running
  char     error[48];             // why the last check or install failed ("" = it did not); cleared by the next try
} bd_release;
const bd_release *bd_release_info(void);
uint32_t          bd_seq_release(void);
void              bd_release_check(void);       // look now (a check already running: nothing)
bd_result         bd_release_install(void);
// interfaces (BD_HAS_UI_CATALOG): the UIs a release offers on this chip, from the same check (Brewdeck's own is
// "screen"; the others are the UIs listed in Brewdeck's uis.json, built and signed by its release workflow). Installing
// one replaces the whole firmware (same core, that UI) like a release install; its settings are its own, Wi-Fi and the
// machine address are kept. If a UI offers no way back, holding the screen for 5 s while it starts boots the previous
// firmware. n = 0 until a check answered (or on a board without updates over Wi-Fi).
#define BD_MAX_UIS 8
typedef struct {
  char     name[16];              // its BD_UI name
  char     title[24];
  char     about[96];             // one line about it ("" = none)
  uint32_t size;                  // image bytes
  bool     current;               // the UI running now
  bool     has_preview;           // the release has a picture of its Ready screen (bd_ui_preview)
} bd_ui_entry;
typedef struct {
  char        version[16];        // the release they come from
  int         n;
  bd_ui_entry ui[BD_MAX_UIS];
} bd_catalog;
const bd_catalog *bd_ui_catalog(void);                // changes with bd_seq_release
bd_result         bd_ui_install(const char *name);    // BUSY / UNKNOWN_ID as bd_release_install; also the running UI
                                                      // (a reinstall); progress: BD_EV_UPDATE
// a picture of an interface's Ready screen (RGB565, 320 x 192), or NULL: not downloaded yet (asking starts the
// download; ask only while you show it) or none in the release. Valid until the next bd_ui_tick (copy it to keep it on
// screen); bd_seq_release changes when one arrives.
typedef struct { const uint16_t *px; int w, h; } bd_picture;
const bd_picture *bd_ui_preview(const char *name);
// key-value storage for the UI's own settings, in the UI's own namespace (flash, survives updates of the UI)
int32_t bd_store_get_int(const char *key, int32_t def);
void    bd_store_set_int(const char *key, int32_t v);
size_t  bd_store_get_blob(const char *key, void *buf, size_t cap);   // the stored size (0 = none); buf filled only if it fits
void    bd_store_set_blob(const char *key, const void *buf, size_t n);
void    bd_store_erase(const char *key);
void    bd_log(const char *fmt, ...);                             // serial log with the time prefix

// ---- display, touch, memory (the board layer behind them; UI task only) --------------------------------------------
typedef enum { BD_PIXEL_RGB565 = 0 } bd_pixel_format;   // native byte order (LVGL 9: LV_COLOR_FORMAT_RGB565, no byte swap)
typedef struct {
  int width, height;              // landscape, as the UI draws: 800 x 480 on both boards (the P4's portrait panel is turned by its board layer)
  bd_pixel_format format;
  int draw_lines;                 // rows of one draw strip (width x draw_lines RGB565) that fit in internal RAM next to
                                  // Wi-Fi: 40 on the S3 (64 KB), 48 on the P4. One strip: the copy is synchronous anyway.
  int row_align;                  // 0 = any rectangle. Else flushed rectangles should start on a multiple of row_align
                                  // rows and be a multiple of it tall (P4: 16; its 2D engine can hang on copies only
                                  // 2-3 rows tall, so thinner ones are copied by the CPU, more slowly). bd_lvgl9.h does it.
} bd_display_info;
const bd_display_info *bd_display(void);
// Puts a finished rectangle on the screen (x2 / y2 inclusive, px row by row, (x2-x1+1) * (y2-y1+1) pixels). Returns
// when px may be reused: the board has copied it into its frame buffer (rotating it on a portrait panel).
void bd_display_flush(int x1, int y1, int x2, int y2, const uint16_t *px);
// One touch point in display coordinates; false = not touched. Read it once per bd_ui_tick (or from LVGL's input
// read callback).
bool bd_touch_read(int *x, int *y);
// Time spent in bd_display_flush (copying to the panel) and pixels flushed since the last reset, for performance logs.
void bd_display_stats(uint32_t *flush_us, uint32_t *flush_px, bool reset);
// Memory: big buffers (chart data, shot traces, images) belong in PSRAM; internal RAM is needed by Wi-Fi and for
// the draw strips. BD_MEM_ANY lets the core pick (PSRAM above 4 KB).
typedef enum { BD_MEM_ANY = 0, BD_MEM_INTERNAL, BD_MEM_PSRAM } bd_mem;
void *bd_alloc(size_t n, bd_mem where);          // NULL when it does not fit
void  bd_free(void *p);

// ---- debug builds only (no-ops in a release build) ----------------------------------------------------------------
// The debug web server (http://<screen>/screen.bmp, /heap, ...) can answer one more GET path for the UI, e.g. "/bench".
// fn runs on the UI task (between two bd_ui_tick calls), writes plain text into out and returns its length; the
// request waits up to 30 s for it.
typedef size_t (*bd_debug_fn)(char *out, size_t cap);
void bd_debug_route(const char *path, bd_debug_fn fn);

// ---- events (core -> UI, on the UI task) --------------------------------------------------------------------------
typedef enum {
  BD_EV_SHOT_START,         // a shot began (its first point, or the machine reports brewing): the live shot is reset
  BD_EV_SHOT_POINT,         // a new point of the running shot (~7/s)
  BD_EV_SHOT_END,           // the machine stopped brewing and no new point came for 0.3 s, or none came for 3 s (15 s
                            // while the machine still reports brewing); a later point of the same shot carries on
                            // with it, without a new SHOT_START
  BD_EV_NOTICE,             // a machine notification (not the pressure release, not the repeated "Fill the water tank!"). The update
                            // notice repeats every 2 min: update = true marks it (bd_machine.update has the release)
  BD_EV_PRESSURE_RELEASE,   // "Dropping beats in: N" (count >= 0) / "Releasing pressure" (count -1)
  BD_EV_COMMAND,            // the machine's answer to a command
  BD_EV_SHOT_LOADED,        // answer to bd_load_previous_shot / bd_load_stored_shot
  BD_EV_WIFI_SCAN,
  BD_EV_WIFI_JOIN,
  BD_EV_UPDATE,             // a firmware update being written (a release, bd_release_install; debug builds also a push
                            // from the computer, build.sh --ota). The screen should say
                            // so above everything (and stay on): RECEIVING with progress, then INSTALLED (the core
                            // restarts ~2 s later) or STOPPED (nothing changed); NONE = nothing to show any more
} bd_event_type;
typedef enum { BD_NOTICE_INFO = 0, BD_NOTICE_SUCCESS, BD_NOTICE_WARN, BD_NOTICE_ERROR } bd_notice_type;
typedef enum { BD_CMD_SELECT = 0, BD_CMD_STOP_WEIGHT, BD_CMD_BREW_TEMP, BD_CMD_FLUSH, BD_CMD_TARE } bd_command;
// OK: the machine reports the change (pick, flush mode, stop weight, brew temperature, scale at ~0 after a tare);
// NOT_TAKEN: it did not within 5 s (7 s for weight / temperature, sent 1 s after the last change). REFUSED and TIMEOUT
// are reserved: this core does not send them (a refusal it knows of is the command's own result).
typedef enum { BD_ANSWER_OK = 0, BD_ANSWER_REFUSED, BD_ANSWER_TIMEOUT, BD_ANSWER_NOT_TAKEN } bd_answer;
typedef enum { BD_LOAD_NONE = 0, BD_LOAD_LOADING, BD_LOAD_OK, BD_LOAD_MISSING, BD_LOAD_FAILED } bd_load;
typedef enum { BD_JOIN_IDLE = 0, BD_JOIN_BUSY, BD_JOIN_OK, BD_JOIN_FAILED } bd_join;
typedef enum { BD_UPDATE_NONE = 0, BD_UPDATE_RECEIVING, BD_UPDATE_INSTALLED, BD_UPDATE_STOPPED } bd_update;
#define BD_HAS_UPDATE_EVENT 1   // additions within API 1.x come with a macro: a UI can still build against an older core
#define BD_HAS_RELEASES 1       // bd_release_*, bd_event.u.update.version
#define BD_HAS_UI_CATALOG 1     // bd_ui_catalog, bd_ui_install

typedef struct {
  bd_event_type type;
  union {
    const bd_shot_point *point;                                   // SHOT_POINT
    struct { bd_notice_type type; const char *text; bool update; } notice;   // NOTICE (update: the firmware-update notice)
    struct { int count; } release;                                // PRESSURE_RELEASE
    struct { bd_command cmd; bd_answer answer; } command;         // COMMAND
    struct { bool history;                                        // SHOT_LOADED: bd_load_stored_shot (true) or
             bd_load state; const bd_shot *shot;                  //   bd_load_previous_shot; shape = the profile
             const bd_profile_shape *shape;                       //   stored with that shot (history only)
             const bd_shot *previous; bd_load previous_state; } loaded;
    struct { const bd_wifi_net *nets; int n; bool scanning; } scan;   // WIFI_SCAN, strongest first
    struct { bd_join state; } join;                               // WIFI_JOIN
    struct { bd_update state; int percent;                        // UPDATE: percent received (STOPPED: where it stopped)
             uint32_t received, total;                            //   bytes
             const char *built;                                   //   the new firmware's build date ("24 Nov 2026 21:40", "" = not known)
             const char *from;                                    //   the sender's address or "github.com" ("" = not known)
             const char *version;                                 //   the release being installed ("" = a debug push)
             const char *title; } update;                         //   the interface being installed ("" = not known)
  } u;
} bd_event;

// ---- what a UI implements -----------------------------------------------------------------------------------------
typedef struct {
  const char *name;         // shown in the log and Menu-like places; also the storage namespace (max 15 chars)
  const char *version;
  int         api_version;  // the API version the UI was written for, as a literal (not BD_API_VERSION)
} bd_ui_desc;
extern const bd_ui_desc BD_UI;
void bd_ui_init(void);                      // once, after the display, touch and the core are up (build the screens)
void bd_ui_tick(uint32_t now_ms);           // every loop pass (~5 ms): draw and flush what changed (LVGL: lv_timer_handler)
void bd_ui_event(const bd_event *e);
void bd_ui_wake(void);                      // the core wants the screen on: a shot, steam or a flush started, a notice or the
                                            // pressure release came (not the repeated update notice)

#ifdef __cplusplus
}
#endif
