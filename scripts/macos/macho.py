"""Shared Mach-O inspection. Requires the pinned macholib 1.16.4 host package."""
import hashlib
from pathlib import Path
import struct

from macholib.MachO import MachO
from macholib.mach_o import (LC_BUILD_VERSION, LC_CODE_SIGNATURE, LC_ID_DYLIB,
                            LC_RPATH, LC_VERSION_MIN_MACOSX)

ARM64 = 0x0100000C
MAGICS = {b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}


def is_macho(path):
    if path.is_symlink() or not path.is_file():
        return False
    with path.open('rb') as stream:
        return stream.read(4) in MAGICS


def inspect(path):
    headers = MachO(str(path)).headers
    result = {'architectures': [h.header.cputype for h in headers],
              'subtypes': [h.header.cpusubtype & 0xFFFFFF for h in headers], 'dependencies': [],
              'rpaths': [], 'id': None, 'minimum': 0, 'platform': None}
    for header in headers:
        if header.header.cputype != ARM64:
            continue
        result['dependencies'] = [name for _, _, name in header.walkRelocatables()]
        for command, value, data in header.commands:
            if command.cmd == LC_ID_DYLIB:
                result['id'] = data.rstrip(b'\0').decode()
            elif command.cmd == LC_RPATH:
                result['rpaths'].append(data.rstrip(b'\0').decode())
            elif command.cmd in {LC_BUILD_VERSION, LC_VERSION_MIN_MACOSX}:
                result['minimum'] = value.minos if command.cmd == LC_BUILD_VERSION else value.version
                result['platform'] = value.platform if command.cmd == LC_BUILD_VERSION else 1
    return result


def system_reference(reference):
    return reference.startswith(('/System/Library/', '/usr/lib/'))


def version_number(version):
    fields = [int(part) for part in version.split('.')]
    fields += [0] * (3 - len(fields))
    return (fields[0] << 16) | (fields[1] << 8) | fields[2]


def code_pages_valid(path):
    """Independent ad-hoc CodeDirectory digest check, not Apple trust validation."""
    blob = path.read_bytes()
    headers = MachO(str(path)).headers
    if len(headers) != 1:
        raise ValueError(f'Expected thin arm64 signature: {path}')
    signatures = [v for c, v, _ in headers[0].commands if c.cmd == LC_CODE_SIGNATURE]
    if len(signatures) != 1:
        raise ValueError(f'Missing signature: {path}')
    sig = signatures[0]
    payload = blob[sig.dataoff:sig.dataoff + sig.datasize]
    magic, length, count = struct.unpack_from('>III', payload)
    if magic != 0xFADE0CC0 or length > len(payload):
        raise ValueError(f'Invalid signature: {path}')
    for index in range(count):
        slot, offset = struct.unpack_from('>II', payload, 12 + index * 8)
        if slot != 0:
            continue
        cd = payload[offset:]
        magic, length, _, flags, hash_offset, _, special, slots, limit = struct.unpack_from('>9I', cd)
        size, kind, _, shift = struct.unpack_from('>4B', cd, 36)
        if magic != 0xFADE0C02 or not flags & 2 or kind not in {1, 2}:
            raise ValueError(f'Invalid ad-hoc CodeDirectory: {path}')
        page_size = 1 << shift
        if slots != (limit + page_size - 1) // page_size:
            raise ValueError(f'Invalid signature page count: {path}')
        digest = hashlib.sha256 if kind == 2 else hashlib.sha1
        for page in range(slots):
            actual = digest(blob[page * page_size:min((page + 1) * page_size, limit)]).digest()[:size]
            if actual != cd[hash_offset + page * size:hash_offset + (page + 1) * size]:
                raise ValueError(f'Invalid signature code digest: {path}, page {page}')
        return {'cdhash': digest(cd[:length]).digest()[:20],
                'digest': digest, 'hash_size': size,
                'special': {slot: cd[hash_offset - slot * size:hash_offset - (slot - 1) * size]
                            for slot in range(1, special + 1)}}
    raise ValueError(f'Missing primary CodeDirectory: {path}')
