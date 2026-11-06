"""Decrypt the AwoX mesh traffic in a PacketLogger capture.

Usage (run in your own Terminal; credentials are prompted, never stored):
    <venv>/bin/python tools/decode_capture.py awox.pklg awox-decoded.txt

Writes one line per frame: time, direction, wire flag, decrypted 16-byte block,
CRC check, and any unencrypted tail. Credentials, randoms and the session key
are not written. Uses the upstream awox-mesh protocol code.
"""
import getpass
import os
import sys
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.expanduser(
    '~/Documents/Codex/2026-11-05/referenced-chatgpt-conversation-this-is-an/work/awox-mesh/src'))

from awox_mesh import protocol as ap  # noqa: E402
from pklg import att_packets  # noqa: E402

PAIR_HANDLE, COMMAND_HANDLE, STATUS_HANDLE = 0x001E, 0x0015, 0x0012


def main(capture, out_path):
    packets = list(att_packets(capture))
    name = getpass.getpass('Mesh name (hidden): ').encode()
    password = getpass.getpass('Mesh password (hidden): ').encode()
    combined = ap.combine_credentials(name, password)

    # One session key per login: a capture can hold several connections (lamps),
    # and a connection can log in again. The key applies to the frames after its login.
    keys, pending, matched = {}, {}, 0
    for i, (_, _, conn, op, handle, value) in enumerate(packets):
        if op == 'write_req' and handle == PAIR_HANDLE and value[:1] == b'\x0c':
            pending[conn] = value
        elif op == 'read_rsp' and conn in pending and value[:1] == b'\x0d':
            pair = pending.pop(conn)
            if ap.att_encrypt(pair[1:9], combined)[:8] != pair[9:17]:
                continue
            matched += 1
            keys[i] = (conn, ap.make_session_key(name, password, pair[1:9], value[1:9]))
    del name, password, combined
    if not matched:
        sys.exit('These credentials do not match any login in the capture. Nothing written.')

    lines, current = [], {}
    for i, (ts, direction, conn, op, handle, value) in enumerate(packets):
        if i in keys:
            current[keys[i][0]] = keys[i][1]
            lines.append(f'--- login on connection {conn:#x}')
        if not ((op == 'write_req' and handle == COMMAND_HANDLE) or (op == 'notify' and handle == STATUS_HANDLE)):
            continue
        if len(value) < 17 or conn not in current:
            continue
        block = ap.att_decrypt(current[conn], value[1:17])
        # Command frames: CRC over block[1 : 1+len+1]; query frames: CRC over [len] + payload.
        n = block[1]
        crc_ok = n + 2 <= 16 and ap.crc8(block[1:n + 2]) == block[0]
        t = datetime.fromtimestamp(ts).strftime('%H:%M:%S.%f')[:-3]
        lines.append(f'{t} conn={conn:#x} {direction} flag={value[0]:02x} block={block.hex()} crc={"ok" if crc_ok else "?"}'
                     + (f' tail={value[17:].hex()}' if len(value) > 17 else ''))
    keys = current = None
    with open(out_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'Decrypted {matched} login(s); wrote {len(lines)} lines to {out_path}.')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
