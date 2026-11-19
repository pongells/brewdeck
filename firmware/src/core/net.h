#pragma once
// Wi-Fi and the Gaggiuino REST API: GETs of the status, the profiles and stored shots, and the one REST command,
// profile select (POST). WebSocket data comes in through net_ws.h; the WebSocket commands are in ws.h.
#include "live.h"
#include "pb.h"

// Starts Wi-Fi and the background task (with no network set up, it waits for one chosen on the screen).
// Call BEFORE board_begin(): Wi-Fi start touches NVS flash, which crashes if the RGB panel is already running.
void net_begin(void);
// Copies the latest data. Returns true when something changed since the last call.
bool net_get(LiveData *out, int *link);
// Copies the profile list only when it changed since the last call (then returns true).
bool net_get_profiles(ProfileList *out);
// Copies the active profile's phases only when they changed since the last call (then returns true).
bool net_get_shape(bd_profile_shape *out);
// Queues POST /api/profile-select/<id>; the network task sends it only if the machine is idle in HOME.
void net_select_profile(int id);
// Next shot snapshot from the WebSocket (~7/s during a shot), oldest first; false when none is waiting.
bool net_get_shot(WsShot *out);
// Shot review: ask for the previous stored shot of this profile (the shot just made: its profile name and duration).
enum { PREV_NONE = BD_LOAD_NONE, PREV_LOADING = BD_LOAD_LOADING, PREV_READY = BD_LOAD_OK, PREV_MISSING = BD_LOAD_MISSING,
       PREV_FAILED = BD_LOAD_FAILED };
void net_request_prev(const char *profile, int dur10);
// Current state (PREV_*); *changed is true once per change, and on PREV_READY the shot is copied to out then.
int  net_get_prev(bd_shot *out, bool *changed);
// The machine's latest notification (d_notif over the WebSocket), copied once per new message (then true).
bool net_get_notif(WsNotif *out);
// Shot history (menu): the newest 50 stored shots (copied only when changed, then true), and loading one of them
// (GET /api/shots/<id> of it and of the next older shot of its profile, only while idle).
bool net_get_index(WsShotIndex *out);
void net_request_hist(uint32_t id);
// State of the last request (PREV_*); on *changed with PREV_READY the shot, its stored profile and (prev_state
// PREV_READY) the shot it is compared with are copied.
int  net_get_hist(bd_shot *shot, bd_profile_shape *shape, bd_shot *prev, int *prev_state, bool *changed);

// Wi-Fi join (menu > Wi-Fi). Scan: results via net_get_wifi_scan (true once per change). Join: tries the network for
// up to 20 s; on success it is saved on the screen (NVS "wifi") and used first at boot, on failure the previous network
// is joined again. The password is never logged.
void net_wifi_scan(void);
bool net_get_wifi_scan(bd_wifi_net *out, int *n, bool *scanning);
void net_wifi_join(const char *ssid, const char *pass);
// "Forget this network": deletes the saved network, leaves Wi-Fi and does not use the built-in one until a network
// is joined from the list (NVS "wifi"/"nobuiltin"). The screen then shows "No Wi-Fi set up".
void net_wifi_forget(void);
bool net_get_wifi_join(int *state);
// Machine address (IP or name, mDNS for names; ".local" optional): saved on the screen (NVS "machine"), used at once.
void net_set_machine(const char *host);
void net_get_machine(char *out, size_t cap);
void net_wifi_retry(void);   // "Try again" on the Wi-Fi failed screen
void net_roam_check(void);   // debug (GET /roam): look for a stronger AP of the same network now, move if clearly better
int net_offline_reason(char *ip, size_t cap);   // WHY_* (live.h); ip = the address that did not answer
void net_wifi_target(char *out, size_t n);   // network the screen tries to join ("" = none set up)
// For the offline screen: the screen's own IP ("" without Wi-Fi) and the machine's address (IP once found, else the
// mDNS name it looks for).
void net_addr_info(char *screen, size_t scap, char *machine, size_t mcap);
// The joined network as the network task last read it (about once a second; on the ESP32-P4 every Wi-Fi query is a
// request to the C6 over SDIO, which can block, so the UI task never asks Wi-Fi itself). False when not connected.
bool net_wifi_info(char *ssid, size_t scap, int8_t *rssi, int *channel, char *ap, size_t acap);
// joined or not, and the signal (any task; the network task's last reading, at most 1 s old)
bool net_wifi_up(int8_t *rssi);
