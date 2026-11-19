#pragma once
// Internal to the network code: net.cpp (the network task: finding the machine, REST), wifi.cpp (the screen's own
// Wi-Fi) and net_data.cpp (what the network and WebSocket tasks hand to the core loop). Not for the UI or the rest
// of the core: they use net.h / net_ws.h.
#include <Arduino.h>
#include <IPAddress.h>
#include "live.h"
#include "pb.h"

// one lock for everything handed between the network task, the WebSocket task and the core loop
extern SemaphoreHandle_t g_net_mtx;
struct NetLock {                     // holds the lock for its scope
  NetLock() { xSemaphoreTake(g_net_mtx, portMAX_DELAY); }
  ~NetLock() { xSemaphoreGive(g_net_mtx); }
  NetLock(const NetLock &) = delete;
  NetLock &operator=(const NetLock &) = delete;
};
void net_wake(void);                 // net.cpp: wake the network task (a request from the UI task)

// net_data.cpp: published by the network task, taken by the core loop (net_get*)
void data_init(void);                                // creates the lock: first thing in net_begin
void data_publish(const LiveData *d, int link);      // d NULL: the link state only
void data_set_why(int why, IPAddress ip);            // offline reason (WHY_*) and the address that did not answer
void data_publish_profiles(const ProfileList *l);
void data_publish_shape(const bd_profile_shape *s);      // the core loop hears of it only when it changed
bool data_index(WsShotIndex *out);                   // a copy of the shot history index; false = none yet
uint32_t data_newest_shot(void);                     // newest stored shot id in the index, 0 = none
uint32_t data_last_shot_ms(void);                    // millis() of the last shot snapshot

// wifi.cpp: network task only
void wifi_setup(void);               // net_begin: credentials, Wi-Fi settings, events (before the panel starts)
void wifi_start(void);               // the first join, when the task starts
// one pass: the link status for the UI, scans, a join from the menu, forget, try again, the built-in network after
// 30 s, roaming (idle: nothing running on the machine, no shot review or lookup). True = the link was restarted (a
// join or forget ran): look the machine up again.
bool wifi_step(bool idle);
void wifi_info_step(bool now = false);   // the link status for the UI: at most once a second unless now
bool wifi_down_why(int *why);        // not connected: true and why (WHY_WIFI_*)
bool wifi_screen_ip(char *out, size_t cap);   // the screen's own IP ("" and false without Wi-Fi)
