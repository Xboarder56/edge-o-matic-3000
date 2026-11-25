#!/usr/bin/env python3
"""Radio settings lock: Wi-Fi/Bluetooth TX power and PHY settings must not change.

Maus-Tec keeps the radios within licensed limits. This firmware fork must not change
them, so CI checks:
  * sdkconfig.defaults against tools/radio-config.lock (before the build), and
  * the effective build config (.pio/build/<env>/config/sdkconfig.h) with --built ENV, and
  * that no firmware source calls runtime radio power/country/protocol APIs.

usage: radio_lock.py [--built ENV]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, "tools", "radio-config.lock")
FORBIDDEN = re.compile(
    r"\b(esp_wifi_set_max_tx_power|esp_wifi_set_country\w*|esp_wifi_set_protocol|"
    r"esp_wifi_set_bandwidth|esp_ble_tx_power_set\w*|esp_bredr_tx_power_set|"
    r"ble_hs_hci_util_set_tx_power|esp_phy_\w+)\s*\("
)
SOURCE_DIRS = ["src", "include", "lib/mt-actions/src", "lib/mt-actions/include"]


def lock():
    wanted = {}
    for line in open(LOCK):
        line = line.strip()
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            wanted[key] = value
    return wanted


def defaults_config():
    config = {}
    for line in open(os.path.join(ROOT, "sdkconfig.defaults")):
        line = line.strip()
        m = re.match(r"^(CONFIG_\w+)=(.*)$", line)
        if m:
            config[m.group(1)] = m.group(2).strip('"')
        m = re.match(r"^# (CONFIG_\w+) is not set$", line)
        if m:
            config[m.group(1)] = "n"
    return config


def built_config(env):
    path = os.path.join(ROOT, ".pio", "build", env, "config", "sdkconfig.h")
    config = {}
    for line in open(path):
        m = re.match(r"^#define (CONFIG_\w+) (.*)$", line.strip())
        if m:
            value = m.group(2).strip('"')
            config[m.group(1)] = "y" if value == "1" else value
    # Deprecated names are defined as aliases of the new ones (#define OLD NEW): resolve them.
    for key, value in list(config.items()):
        seen = set()
        while value in config and value not in seen:
            seen.add(value)
            value = config[value]
        config[key] = value
    return config


def compare(label, config, wanted, absent_means_n):
    problems = []
    for key, value in wanted.items():
        actual = config.get(key, "n" if absent_means_n else None)
        if label == "build" and value == "y" and actual == "1":
            actual = "y"
        if actual != value:
            problems.append("%s: %s is %r, locked to %r" % (label, key, actual, value))
    return problems


def main():
    wanted = lock()
    problems = compare("sdkconfig.defaults", defaults_config(), wanted, absent_means_n=False)

    if "--built" in sys.argv:
        env = sys.argv[sys.argv.index("--built") + 1]
        built = built_config(env)
        # Only check keys this IDF version defines (deprecated aliases may be absent).
        present = {k: v for k, v in wanted.items() if k in built or v == "n"}
        problems += compare("build", built, present, absent_means_n=True)

    for d in SOURCE_DIRS:
        for base, _, files in os.walk(os.path.join(ROOT, d)):
            for name in files:
                if not name.endswith((".c", ".h", ".cpp", ".hpp")):
                    continue
                path = os.path.join(base, name)
                text = open(path, errors="replace").read()
                for m in FORBIDDEN.finditer(text):
                    line = text.count("\n", 0, m.start()) + 1
                    problems.append("radio API call: %s:%d (%s)"
                                    % (os.path.relpath(path, ROOT), line, m.group(1)))

    if problems:
        for p in problems:
            print("RADIO: " + p, file=sys.stderr)
        print("Radio settings lock FAILED.", file=sys.stderr)
        return 1
    print("Radio settings lock OK (%d settings%s, no runtime radio calls)"
          % (len(wanted), ", checked in the build" if "--built" in sys.argv else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
