"""Minimal PacketLogger (.pklg) reader for BLE ATT traffic.

Record: uint32 LE length (of what follows), uint32 secs, uint32 usecs, uint8 type, payload.
Types used here: 0x02 ACL sent (phone -> lamp), 0x03 ACL received (lamp -> phone).
"""
import struct
import sys
from datetime import datetime, timezone

ATT_OPS = {0x0A: 'read_req', 0x0B: 'read_rsp', 0x12: 'write_req', 0x13: 'write_rsp',
           0x52: 'write_cmd', 0x1B: 'notify', 0x1D: 'indicate', 0x01: 'error'}


def records(path):
    data = open(path, 'rb').read()
    pos = 0
    while pos + 4 <= len(data):
        (length,) = struct.unpack_from('<I', data, pos)
        if length < 9 or pos + 4 + length > len(data):
            break
        secs, usecs, kind = struct.unpack_from('<IIB', data, pos + 4)
        yield secs + usecs / 1e6, kind, data[pos + 13:pos + 4 + length]
        pos += 4 + length


def att_packets(path):
    """Yield (time, direction, conn_handle, opcode_name, att_handle, value) for single-fragment ATT PDUs."""
    for ts, kind, payload in records(path):
        if kind not in (0x02, 0x03) or len(payload) < 9:
            continue
        conn, acl_len = struct.unpack_from('<HH', payload, 0)
        pb = (conn >> 12) & 0x3
        if pb == 0x1:  # continuation fragment; lamp PDUs are small enough not to need these
            continue
        l2_len, cid = struct.unpack_from('<HH', payload, 4)
        if cid != 0x0004:
            continue
        att = payload[8:8 + l2_len]
        op = att[0]
        name = ATT_OPS.get(op, hex(op))
        direction = 'phone->lamp' if kind == 0x02 else 'lamp->phone'
        handle, value = None, att[1:]
        if op in (0x0A, 0x12, 0x52, 0x1B, 0x1D):
            handle, value = struct.unpack_from('<H', att, 1)[0], att[3:]
        yield ts, direction, conn & 0x0FFF, name, handle, value


if __name__ == '__main__':
    for ts, direction, conn, name, handle, value in att_packets(sys.argv[1]):
        t = datetime.fromtimestamp(ts, timezone.utc).astimezone().strftime('%H:%M:%S.%f')[:-3]
        h = '' if handle is None else f' h=0x{handle:04x}'
        print(f'{t} {direction} conn={conn:#x} {name}{h} len={len(value)} {value[:1].hex()}…')
