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
    pair_write = next(v for _, _, _, n, h, v in packets if n == 'write_req' and h == PAIR_HANDLE)
    pair_reply = next(v for _, _, _, n, _, v in packets if n == 'read_rsp' and v[:1] == b'\x0d')
    client_random, proof, server_random = pair_write[1:9], pair_write[9:17], pair_reply[1:9]

    name = getpass.getpass('Mesh name (hidden): ').encode()
    password = getpass.getpass('Mesh password (hidden): ').encode()
    combined = ap.combine_credentials(name, password)
    if ap.att_encrypt(client_random, combined)[:8] != proof:
        sys.exit('These credentials do not match the login in the capture. Nothing written.')
    key = ap.make_session_key(name, password, client_random, server_random)
    del name, password, combined

    lines = []
    for ts, direction, _, op, handle, value in packets:
        if not ((op == 'write_req' and handle == COMMAND_HANDLE) or (op == 'notify' and handle == STATUS_HANDLE)):
            continue
        if len(value) < 17:
            continue
        block = ap.att_decrypt(key, value[1:17])
        # Command frames: CRC over block[1 : 1+len+1]; query frames: CRC over [len] + payload.
        n = block[1]
        crc_ok = n + 2 <= 16 and ap.crc8(block[1:n + 2]) == block[0]
        t = datetime.fromtimestamp(ts).strftime('%H:%M:%S.%f')[:-3]
        lines.append(f'{t} {direction} flag={value[0]:02x} block={block.hex()} crc={"ok" if crc_ok else "?"}'
                     + (f' tail={value[17:].hex()}' if len(value) > 17 else ''))
    key = None
    with open(out_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'Wrote {len(lines)} frames to {out_path}.')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
