#!/usr/bin/env python3
"""Set up the lamp bridge over USB: Wi-Fi and mesh credentials, plus the lamp list.

    python3 setup.py [serial-port]

The credentials are asked for here (passwords hidden), sent over the USB cable and kept only in the
board's flash. Nothing is written on the Mac. Leave an answer empty to keep the board's current value.
"""
import getpass
import glob
import os
import sys
import time

try:
    import serial
except ImportError:  # PlatformIO ships pyserial in its own Python (Homebrew install or ~/.platformio)
    import shutil
    pio = shutil.which('pio')
    candidates = [os.path.join(os.path.dirname(os.path.realpath(pio)), 'python')] if pio else []
    candidates.append(os.path.expanduser('~/.platformio/penv/bin/python'))
    for python in candidates:
        if os.path.exists(python) and os.path.realpath(sys.executable) != os.path.realpath(python):
            os.execv(python, [python] + sys.argv)
    sys.exit('pyserial is missing: pip3 install pyserial')

HERE = os.path.dirname(os.path.abspath(__file__))


def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    ports = sorted(set(sum((glob.glob(p) for p in (
        '/dev/cu.usbserial*', '/dev/cu.SLAB_USBtoUART*', '/dev/cu.wchusbserial*', '/dev/cu.usbmodem*')), [])))
    if not ports:
        sys.exit('No board found. Plug it in with a data cable (the Kindle cable works).')
    if len(ports) > 1:
        sys.exit('Several serial ports: ' + ', '.join(ports) + '. Pass the right one as an argument.')
    return ports[0]


def command(port, line, timeout=5):
    """Send one console line and wait for its ok/error reply; log lines in between are shown."""
    port.write((line + '\n').encode())
    end = time.time() + timeout
    while time.time() < end:
        reply = port.readline().decode(errors='replace').strip()
        if reply.startswith('ok') or reply.startswith('error'):
            return reply
        if reply.startswith('['):
            print('  ' + reply)
    return 'error no reply'


def main():
    name = find_port()
    print('Board on ' + name + '. Opening it restarts the board; waiting for it to boot…')
    port = serial.Serial(name, 115200, timeout=0.5)
    time.sleep(3)
    port.reset_input_buffer()
    state = command(port, 'info')
    if not state.startswith('ok'):
        sys.exit('The board does not answer (' + state + '). Is the bridge firmware flashed? (pio run -t upload)')
    print('Board reports: ' + state[3:])

    answers = [
        ('wifi_ssid', input('Wi-Fi name (empty = keep): ')),
        ('wifi_pass', getpass.getpass('Wi-Fi password (hidden, empty = keep): ')),
        ('mesh_name', input('Mesh name (empty = keep): ')),
        ('mesh_pass', getpass.getpass('Mesh password (hidden, empty = keep): ')),
    ]
    for key, value in answers:
        if value:
            result = command(port, 'set ' + key + ' ' + value.encode().hex())
            print(('  ' + key + ': stored') if result.startswith('ok') else ('  ' + key + ': ' + result))
    answers.clear()

    # Password for wireless updates (ota.sh); created once, kept outside the repo.
    ota_file = os.path.expanduser('~/.config/lampbridge/ota_password')
    if not os.path.exists(ota_file):
        import secrets
        os.makedirs(os.path.dirname(ota_file), mode=0o700, exist_ok=True)
        with open(os.open(ota_file, os.O_WRONLY | os.O_CREAT, 0o600), 'w') as f:
            f.write(secrets.token_urlsafe(18))
    with open(ota_file) as f:
        result = command(port, 'set ota_pass ' + f.read().strip().encode().hex())
    print('  wireless updates: ' + ('enabled (./ota.sh)' if result.startswith('ok') else result))

    lamps = os.path.join(HERE, '..', 'lamps.json')
    if os.path.exists(lamps):
        with open(lamps, 'rb') as f:
            result = command(port, 'set lamps_json ' + f.read().hex())
        print('  lamp list: ' + ('stored (the bridge also fetches it from GitHub Pages)' if result.startswith('ok') else result))

    print('Restarting the board…')
    command(port, 'restart')
    end = time.time() + 40
    while time.time() < end:
        line = port.readline().decode(errors='replace').strip()
        if line.startswith('['):
            print('  ' + line)
        if 'Connected (MTU' in line:
            break
    print('Done. Open the address shown above on the phone, the Mac or the Kindle (/k).')


if __name__ == '__main__':
    main()
