# PlatformIO pre-script, for envs with custom_sdkconfig (they recompile the ESP-IDF libraries, "hybrid compile").
# The Arduino core lists its ESP-IDF components with version ranges (idf_component.yml, e.g. esp_hosted ^2.12.13), so
# a recompile takes the newest version each range allows, while the app is still compiled against the headers of the
# stock libraries. The two then disagree: on the P4 Wi-Fi did not start (esp_wifi_init: ESP_ERR_INVALID_ARG) or never
# joined (ESP-Hosted newer than the C6's firmware). This pins every listed component to the exact version the stock
# libraries were built with (their dependencies.lock), so a recompile builds the same components as the stock build.
import os
import re
Import("env")

if env.GetProjectOption("custom_sdkconfig", ""):
    pkg = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    libs = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
    mcu = env.BoardConfig().get("build.mcu", "")
    manifest = os.path.join(pkg or "", "idf_component.yml")
    lock = os.path.join(libs or "", mcu, "dependencies.lock")
    if os.path.isfile(manifest) and os.path.isfile(lock):
        stock, name = {}, None
        for line in open(lock, encoding="utf-8"):
            m = re.match(r"^  ([\w./-]+):\s*$", line)
            if m:
                name = m.group(1)
                continue
            m = re.match(r"^    version: (.+)$", line)
            if m and name:
                stock[name] = m.group(1).strip().strip('"')
                name = None
        out, name, changed = [], None, 0
        for line in open(manifest, encoding="utf-8"):
            m = re.match(r"^  ([\w./-]+):\s*$", line)
            if m:
                name = m.group(1)
            elif not line.startswith("    ") and line.strip():
                name = None                                  # left the entry (or the dependencies block)
            m = re.match(r'^(    version: )"?([^"\n]+)"?\s*$', line)
            if m and name in stock and m.group(2) != stock[name]:
                line = '%s"%s"\n' % (m.group(1), stock[name])
                changed += 1
            out.append(line)
        if changed:
            open(manifest, "w", encoding="utf-8").writelines(out)
            print("pin_components: %d ESP-IDF components pinned to the stock libraries' versions (%s)" % (changed, mcu))
