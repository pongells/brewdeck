#!/usr/bin/env bash
# Logs the screen's Wi-Fi signal for a while, to compare antennas or places: ./tools/rssi_log.sh <label> [minutes] [every_s]
# Env HOST (default brewdeck.local; the ESP32-P4 board: HOST=brewdeck-p4.local), OUT (folder, default .). Needs a debug
# build (GET /heap). Writes <OUT>/rssi-<label>.csv (time, rssi, channel, access point) and prints the median, the spread
# and how many reads failed; with several CSVs in OUT it also prints them side by side. Sends nothing to the machine.
set -euo pipefail
HOST="${HOST:-brewdeck.local}"; OUT="${OUT:-.}"
LABEL="${1:?usage: tools/rssi_log.sh <label> [minutes] [every_s]}"; MIN="${2:-5}"; EVERY="${3:-2}"
mkdir -p "$OUT"; CSV="$OUT/rssi-$LABEL.csv"
echo "time,rssi,channel,ap" > "$CSV"
END=$(awk -v n="$(date +%s)" -v m="$MIN" 'BEGIN { print int(n + m * 60) }')   # minutes may be a decimal
echo "Logging $HOST every ${EVERY} s for $MIN min into $CSV (keep the screen still)"
while (( $(date +%s) < END )); do
  T=$(date +%s)
  # -4: macOS otherwise waits ~5 s for an IPv6 answer from mDNS first; a failed read is logged as a gap
  L="$(curl -4 -s -m 3 "http://$HOST/heap" | grep -o 'wifi rssi .*' || true)"
  if [[ -n "$L" ]]; then
    R=$(echo "$L" | sed -n 's/.*rssi \(-*[0-9]*\) dBm.*/\1/p'); C=$(echo "$L" | sed -n 's/.*channel \([0-9]*\).*/\1/p')
    A=$(echo "$L" | sed -n 's/.*ap \([0-9A-Fa-f:]*\).*/\1/p')
    echo "$T,$R,$C,$A" >> "$CSV"
  else
    echo "$T,,," >> "$CSV"
  fi
  sleep "$EVERY"
done
python3 -I - "$OUT" <<'EOF'
import csv, glob, os, statistics, sys
rows = []
for f in sorted(glob.glob(os.path.join(sys.argv[1], 'rssi-*.csv'))):
    r = list(csv.DictReader(open(f)))
    v = sorted(int(x['rssi']) for x in r if x['rssi'])
    if not v: rows.append((os.path.basename(f)[5:-4], len(r), len(r), None)); continue
    q = lambda p: v[min(len(v) - 1, int(p * len(v)))]
    aps = sorted({x['ap'] for x in r if x['ap']})
    rows.append((os.path.basename(f)[5:-4], len(r), len(r) - len(v),
                 (statistics.median(v), q(0.1), q(0.9), v[0], v[-1], len(aps))))
print('\n%-16s %6s %6s %7s %5s %5s %5s %5s %4s' % ('label', 'reads', 'failed', 'median', 'p10', 'p90', 'min', 'max', 'APs'))
for lab, n, fail, s in rows:
    if s is None: print('%-16s %6d %6d   (no answers)' % (lab, n, fail)); continue
    print('%-16s %6d %6d %7.1f %5d %5d %5d %5d %4d' % ((lab, n, fail) + s))
print('dBm, higher is better (-55 is good, -75 weak, -85 barely usable); p10 = the worst tenth of the reads')
EOF
