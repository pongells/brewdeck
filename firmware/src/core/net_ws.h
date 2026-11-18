#pragma once
// Internal: ws.cpp hands decoded WebSocket data to net.cpp (which owns the merged LiveData and the mutex).
#include <IPAddress.h>
#include "pb.h"

bool net_machine_ip(IPAddress *ip);       // the machine's address once the REST task has found it
void net_ws_sensor(const WsSensor *s);
void net_ws_system(const WsSystem *s);
void net_ws_shot(const WsShot *s);
void net_ws_index(const WsShotIndex *ix); // shot history index (on connect and after each shot)
void net_ws_versions(const char *front, const char *stat);   // built-in screen / web UI versions (d_settings)
void net_ws_steam_set(uint32_t deg);         // steam set point from d_settings (nothing else of it is passed on)
void net_ws_scale_cfg(const WsScaleCfg *c); // scale type flags from d_settings (nothing else of it is passed on)
void net_ws_act_profile(const char *name, float weight, float temp);   // active profile (d_act_prof): name, target weight, brew temp
void net_ws_notif(const WsNotif *n);          // d_notif: the machine's notification (shown as a banner)
void net_ws_lost(void);                   // connection gone: REST values only again
// the STORED profile's stop weight and brew temperature (GET /api/profile/<id>), for the save check in
// ws.cpp; called from the WS task, own buffer; false if the machine did not answer or it did not parse
bool net_stored_profile(int id, float *weight, float *temp);
void net_profile_saved(int id);   // ws.cpp: profile id was saved from this screen; its listed values are re-read
