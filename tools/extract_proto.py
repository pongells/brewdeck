#!/usr/bin/env python3
"""Rebuild .proto text from a protobuf-ts bundle's reflection metadata (reads the JS as text, never runs it).

Usage: python3 -I tools/extract_proto.py <web-ui-bundle.js> <out.proto> [<out-schema.json>]
Get the bundle from the machine: http://<machine>/  -> <script src="/assets/index-XXXX.js">  (the ESP32 is slow:
~2.4 KB/s, use curl -C - -H 'Accept-Encoding: gzip' and gunzip). Re-run after every Gaggiuino firmware update.
The output is Gaggiuino's schema (CC BY-NC 4.0, see the header it writes): for your own reference, not committed here.

protobuf-ts emits, per message:  VAR=new class extends Base{constructor(){super(`pkg.Name`,[{no:1,name:`x`,kind:`scalar`,T:2}, ...])}
and per enum field: kind:`enum`,T:()=>[`pkg.EnumName`,ENUMVAR] ; per message field: kind:`message`,T:()=>VAR
"""
import re, sys, json

SCALAR = {1: 'double', 2: 'float', 3: 'int64', 4: 'uint64', 5: 'int32', 6: 'fixed64', 7: 'fixed32', 8: 'bool',
          9: 'string', 12: 'bytes', 13: 'uint32', 15: 'sfixed32', 16: 'sfixed64', 17: 'sint32', 18: 'sint64'}

src = open(sys.argv[1], encoding='utf-8', errors='replace').read()

def match_bracket(s, i):
    """s[i] is '[' or '{' or '('; return index just past the matching close, skipping template/quoted strings."""
    pairs = {'[': ']', '{': '}', '(': ')'}
    stack, q = [], None
    j = i
    while j < len(s):
        c = s[j]
        if q:
            if c == '\\': j += 2; continue
            if c == q: q = None
        elif c in '`"\'': q = c
        elif c in pairs: stack.append(pairs[c])
        elif stack and c == stack[-1]:
            stack.pop()
            if not stack: return j + 1
        j += 1
    return -1

# 1) messages: VAR=new class extends X{constructor(){super(`Name`,[ ...fields... ])
msgs, var2msg = {}, {}
for m in re.finditer(r'([A-Za-z_$][\w$]*)=new class extends [\w$]+\{constructor\(\)\{super\(`([\w.]+)`,\[', src):
    var, name = m.group(1), m.group(2)
    start = m.end() - 1
    end = match_bracket(src, start)
    if end < 0: continue
    msgs[name] = src[start:end]
    var2msg[var] = name

# 2) enums: find T:()=>[`pkg.Enum`,VAR] then the VAR's definition  VAR=function(e){return e[e.A=0]=`A`,...}
enum_vals = {}
def enum_values(var):
    m = re.search(re.escape(var) + r'=function\(e\)\{return (.*?),e\}\(\{\}\)', src)
    if not m: return None
    return [(k, int(v)) for k, v in re.findall(r'e\[e\.(\w+)=(-?\d+)\]', m.group(1))]

def parse_fields(arr):
    out = []
    i = 1
    while True:
        k = arr.find('{', i)
        if k < 0: break
        e = match_bracket(arr, k)
        obj = arr[k:e]
        i = e
        f = {}
        for key in ('no', 'repeat', 'opt'):
            mm = re.search(r'\b' + key + r':(!?\d+)', obj)
            if mm: f[key] = mm.group(1)
        mm = re.search(r'name:`(\w+)`', obj); f['name'] = mm.group(1) if mm else '?'
        mm = re.search(r'kind:`(\w+)`', obj); f['kind'] = mm.group(1) if mm else '?'
        mm = re.search(r'oneof:`(\w+)`', obj); f['oneof'] = mm.group(1) if mm else None
        if f['kind'] == 'scalar':
            mm = re.search(r'\bT:(\d+)', obj); f['type'] = SCALAR.get(int(mm.group(1)), 'T' + mm.group(1)) if mm else '?'
        elif f['kind'] == 'message':
            mm = re.search(r'\bT:\(\)=>([\w$]+)', obj); f['type'] = var2msg.get(mm.group(1), '?' + mm.group(1)) if mm else '?'
        elif f['kind'] == 'enum':
            mm = re.search(r'\bT:\(\)=>\[`([\w.]+)`,([\w$]+)', obj)
            if mm:
                f['type'] = mm.group(1)
                if mm.group(1) not in enum_vals:
                    enum_vals[mm.group(1)] = enum_values(mm.group(2))
            else: f['type'] = '?'
        elif f['kind'] == 'map':
            mk = re.search(r'K:(\d+)', obj); mv = re.search(r'V:\{kind:`(\w+)`,T:(\(\)=>)?([\w$]+)', obj)
            kt = SCALAR.get(int(mk.group(1)), '?') if mk else '?'
            if mv and mv.group(1) == 'scalar': vt = SCALAR.get(int(mv.group(3)), '?')
            elif mv: vt = var2msg.get(mv.group(3), '?' + mv.group(3))
            else: vt = '?'
            f['type'] = f'map<{kt}, {vt}>'
        out.append(f)
    return out

def short(n): return n.split('.')[-1]

lines = ["// Gaggiuino's WebSocket protocol schema, reconstructed from the Gaggiuino web UI bundle (tools/extract_proto.py).",
         '// The original .proto files are part of Gaggiuino (https://github.com/Zer0-bit/gaggiuino), licensed under Creative',
         '// Commons Attribution-NonCommercial 4.0 (https://creativecommons.org/licenses/by-nc/4.0/). This file is therefore',
         "// NOT under Brewdeck's license: it stays under CC BY-NC 4.0, credited to the Gaggiuino project. Keep it for your",
         '// own use; Brewdeck does not ship it (its decoder uses the documented fields plus those in docs/ws/README.md).',
         '// Changes from the original: reassembled from compiled metadata; comments, defaults and package layout differ.',
         '// Protocol documentation: https://gaggiuino.github.io/#/rest-api/websocket',
         '// Reconstructed from the Gaggiuino web UI bundle (protobuf-ts reflection metadata).',
         '// Field numbers/types are exact; comments, defaults and package layout are not recoverable.',
         'syntax = "proto3";', '']
for name in msgs: parse_fields(msgs[name])   # collects enum_vals as a side effect: must run before writing enums
for ename, vals in sorted(enum_vals.items()):
    lines.append(f'enum {short(ename)} {{  // {ename}')
    for k, v in (vals or []): lines.append(f'  {k} = {v};')
    if not vals: lines.append('  // values not found')
    lines += ['}', '']
for name in sorted(msgs):
    fields = parse_fields(msgs[name])
    lines.append(f'message {short(name)} {{  // {name}')
    oneofs = {}
    for f in fields:
        t = f.get('type', '?')
        t = t if t.startswith('map<') else (short(t) if '.' in t else t)
        lab = 'repeated ' if 'repeat' in f else ('optional ' if f.get('opt') in ('!0', '1', 'true') else '')
        line = f'  {lab}{t} {f["name"]} = {f.get("no", "?")};'
        if f.get('oneof'): oneofs.setdefault(f['oneof'], []).append(line.replace(lab, '') if lab == 'optional ' else line)
        else: lines.append(line)
    for o, ls in oneofs.items():
        lines.append(f'  oneof {o} {{')
        lines += ['  ' + l for l in ls]
        lines.append('  }')
    lines += ['}', '']

# action names (string enum NE = function(e){return e.SensorState=`d_sensor_snap`, ...})
acts = re.findall(r'e\.(\w+)=`([dcg]_\w+)`', src)
lines.append('// WebSocketMessageDto.action values (name in the web UI -> wire string):')
seen = set()
for k, v in acts:
    if v in seen: continue
    seen.add(v); lines.append(f'//   {v:24s} {k}')
open(sys.argv[2], 'w').write('\n'.join(lines) + '\n')
if len(sys.argv) > 3:   # machine-readable schema for decoders (e.g. tools/ws_listen.py)
    schema = {'messages': {short(n): parse_fields(a) for n, a in msgs.items()},
              'enums': {short(n): dict((v, k) for k, v in (vals or [])) for n, vals in enum_vals.items()},
              'actions': {v: k for k, v in acts}}
    for fl in schema['messages'].values():
        for f in fl:
            if '.' in f.get('type', '') and not f['type'].startswith('map<'): f['type'] = short(f['type'])
    json.dump(schema, open(sys.argv[3], 'w'), indent=1)
print(f'{len(msgs)} messages, {len(enum_vals)} enums, {len(seen)} actions -> {sys.argv[2]}')
