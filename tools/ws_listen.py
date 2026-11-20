#!/usr/bin/env python3
"""Passive Gaggiuino WebSocket listener: connects to ws://<machine>/ws, decodes what the machine pushes, reports
message types and rates. RECEIVE ONLY: it never sends a WebSocketMessageDto (no commands, no g_* requests); the only
frames it writes are protocol-level pongs and the final close. Note: it occupies one of the machine's 3 WS slots.

Usage: python3 -I tools/ws_listen.py <machine-ip> <schema.json> [seconds=30] [--samples] [--raw frames.bin]
--raw saves every frame as <u32 little-endian length><WebSocketMessageDto bytes> for host tests and playback;
d_settings frames are NEVER saved (they carry MQTT credentials and API tokens).
--times t.txt writes one line per frame saved with --raw: <unix time> <action> (to line frames up with a clock).
schema.json describes the messages to decode, written from Gaggiuino's WebSocket docs
(https://gaggiuino.github.io/rest-api/websocket.md): {"messages": {"<Message>": [{"no": 1, "name": "action",
"kind": "scalar", "type": "string"}, ...]}, "enums": {}, "actions": {}}. For --raw recordings the envelope
(WebSocketMessageDto) alone is enough.
Standard library only.
"""
import base64, json, os, socket, struct, sys, time
from collections import defaultdict

host, schema_path = sys.argv[1], sys.argv[2]
seconds = float(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith('-') else 30.0
show_samples = '--samples' in sys.argv
raw_out = open(sys.argv[sys.argv.index('--raw') + 1], 'wb') if '--raw' in sys.argv else None
times_out = open(sys.argv[sys.argv.index('--times') + 1], 'w') if '--times' in sys.argv else None
S = json.load(open(schema_path))
MSG, ENUM, ACT = S['messages'], S['enums'], S['actions']

# action -> payload message type, as used by the web UI (d_* pushes); unknown actions are shown raw
PAYLOAD = {'d_sensor_snap': 'SensorStateSnapshotDto', 'd_shot_snap': 'ShotSnapshotDto', 'd_sys_state': 'SystemStateDto',
           'd_act_prof': 'ProfileDto', 'd_prof_dict': 'SavedProfilesDto', 'd_prof': 'ProfileDto',
           'd_settings': 'GaggiaSettingsDto', 'd_notif': 'NotificationDto', 'd_log_record': 'LogRecordDto',
           'd_esp_mem': 'EspStatsDto', 'd_resp': 'WebSocketResponseDto', 'd_desc_progr': 'DescalingProgressDto',
           'd_shot_hist_index': 'ShotHistoryIndexDto', 'd_fw_upd_progr': 'FirmwareUpdateProgressDto'}

# ---- minimal protobuf decoding driven by the extracted schema ---------------------------------------------------
def varint(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7F) << s; s += 7
        if c < 0x80: return r, i

def decode(b, mtype):
    fields = {f['no'] if isinstance(f['no'], int) else int(f['no']): f for f in MSG.get(mtype, [])}
    out, i = {}, 0
    while i < len(b):
        key, i = varint(b, i)
        no, wt = key >> 3, key & 7
        f = fields.get(no, {'name': f'#{no}', 'kind': '?', 'type': '?'})
        t, name = f.get('type', '?'), f['name']
        if wt == 0:
            v, i = varint(b, i)
            if t == 'bool': v = bool(v)
            elif f.get('kind') == 'enum': v = ENUM.get(t, {}).get(str(v), ENUM.get(t, {}).get(v, v))
            elif t in ('int32', 'int64') and v >= 1 << 63: v -= 1 << 64
        elif wt == 5:
            raw = b[i:i + 4]; i += 4
            v = struct.unpack('<f', raw)[0] if t == 'float' else struct.unpack('<I', raw)[0]
        elif wt == 1:
            raw = b[i:i + 8]; i += 8
            v = struct.unpack('<d', raw)[0] if t == 'double' else struct.unpack('<Q', raw)[0]
        elif wt == 2:
            n, i = varint(b, i)
            raw = b[i:i + n]; i += n
            if t == 'string': v = raw.decode('utf-8', 'replace')
            elif t == 'bytes': v = raw
            elif t == 'float' and 'repeat' in f: v = list(struct.unpack('<%df' % (n // 4), raw))
            elif t in MSG: v = decode(raw, t)
            else: v = raw
        else:
            raise ValueError(f'wire type {wt}')
        if 'repeat' in f and not (wt == 2 and isinstance(v, list)):
            out.setdefault(name, []).append(v)
        else:
            out[name] = v
    return out

# ---- minimal RFC 6455 client ---------------------------------------------------------------------------------
def send_frame(sock, opcode, payload=b''):
    mask = os.urandom(4)
    hdr = bytes([0x80 | opcode])
    n = len(payload)
    hdr += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n)
    sock.sendall(hdr + mask + bytes(c ^ mask[k % 4] for k, c in enumerate(payload)))

def recv_exact(sock, n):
    buf = b''
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c: raise ConnectionError('closed')
        buf += c
    return buf

sock = socket.create_connection((host, 80), timeout=5)
key = base64.b64encode(os.urandom(16)).decode()
sock.sendall((f'GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
              f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
resp = b''
while b'\r\n\r\n' not in resp:
    c = sock.recv(1)
    if not c: sys.exit('handshake: the machine closed the connection (all 3 WebSocket slots taken?)')
    resp += c
status = resp.split(b'\r\n')[0].decode()
print('handshake:', status)
if ' 101' not in status: sys.exit(1)

stats = defaultdict(lambda: {'n': 0, 'bytes': 0, 'first': None, 'last': None, 't': []})
t0 = time.time()
sock.settimeout(1.0)
frag = b''
try:
    while time.time() - t0 < seconds:
        try:
            h = recv_exact(sock, 2)
        except socket.timeout:
            continue
        fin, op, n = h[0] & 0x80, h[0] & 0x0F, h[1] & 0x7F
        if n == 126: n = struct.unpack('>H', recv_exact(sock, 2))[0]
        elif n == 127: n = struct.unpack('>Q', recv_exact(sock, 8))[0]
        payload = recv_exact(sock, n)
        if op == 9: send_frame(sock, 10, payload); continue          # ping -> pong (protocol only)
        if op == 8: print('server closed the connection'); break
        if op in (0, 2):
            frag += payload
            if not fin: continue
            data, frag = frag, b''
            now = time.time() - t0
            try:
                env = decode(data, 'WebSocketMessageDto')
            except Exception as e:
                print(f'{now:6.2f}s undecodable frame ({len(data)} B): {e}'); continue
            act = env.get('action', '?')
            if raw_out and act != 'd_settings':
                raw_out.write(struct.pack('<I', len(data)) + data)
                if times_out: times_out.write('%.3f %s\n' % (time.time(), act)); times_out.flush()
                raw_out.flush()
            st = stats[act]
            st['n'] += 1; st['bytes'] += len(data); st['t'].append(now)
            body = env.get('data', b'')
            if act == 'd_settings':   # MQTT and Wi-Fi settings, API tokens: never decoded, kept or printed
                dec = {'raw_bytes': len(body), 'not decoded': 'd_settings'}
                if st['first'] is None: st['first'] = dec
                st['last'] = dec
                if st['n'] == 1: print(f'{now:6.2f}s first {act} ({len(data)} B)')
                continue
            try:
                dec = decode(body, PAYLOAD[act]) if act in PAYLOAD else {'raw_bytes': len(body)}
            except Exception as e:
                dec = {'decode_error': str(e), 'raw_bytes': len(body)}
            if st['first'] is None: st['first'] = dec
            st['last'] = dec
            if st['n'] == 1: print(f'{now:6.2f}s first {act} ({len(data)} B)')
finally:
    try: send_frame(sock, 8, struct.pack('>H', 1000))
    except Exception: pass
    sock.close()
    if raw_out: raw_out.close()

dur = time.time() - t0
print(f'\n{dur:.1f} s listened. Per action:')
for act, st in sorted(stats.items(), key=lambda x: -x[1]['n']):
    ts = st['t']
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    rate = st['n'] / dur
    gap = f'gap min {min(gaps)*1000:.0f} / med {sorted(gaps)[len(gaps)//2]*1000:.0f} / max {max(gaps)*1000:.0f} ms' if gaps else ''
    print(f'  {act:16s} {st["n"]:5d} msgs  {rate:6.2f}/s  {st["bytes"]/dur/1024:6.2f} KB/s  avg {st["bytes"]//st["n"]} B  {gap}')
if show_samples:
    for act, st in stats.items():
        SECRET = ('password', 'token', 'pass', 'secret', 'key', 'ssid', 'mqtt', 'user', 'host', 'mac', 'ip')
        def trim(d):   # also redacts credentials / network details (d_settings carries MQTT and Wi-Fi settings, tokens)
            if isinstance(d, list):
                return f'<{len(d)} items>' if len(d) > 6 else [trim(x) for x in d]
            if not isinstance(d, dict): return d
            return {k: ('<redacted>' if any(s in k.lower() for s in SECRET) else trim(v)) for k, v in d.items()}
        print(f'\n--- last {act}:'); print(json.dumps(trim(st['last']), default=str, indent=1)[:1500])
