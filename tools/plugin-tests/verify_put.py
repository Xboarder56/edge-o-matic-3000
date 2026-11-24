"""Rebuild the bytes the device wrote during the last fput (from its echo) and compare."""
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('latin1')
i = raw.rfind('>>>SENDHEX:'); j = raw.find('<<<RECV:', i)
hexs = ''
for ln in raw[raw.find('\n', i) + 1:j].replace('\r', '').split('\n'):
    m = re.match(r'([0-9A-F]*)', ln)   # echo hex, possibly followed by an interleaved log line
    hexs += m.group(1)
dev = bytes.fromhex(hexs[:len(hexs) // 2 * 2]); loc = open(sys.argv[2], 'rb').read()
print('device', len(dev), 'local', len(loc), 'IDENTICAL' if dev == loc else 'DIFFERENT')
