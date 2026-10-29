"""Adapt only the pinned user's proxy entry; retain CRT initialization and NGX APIs."""
from pathlib import Path
import hashlib
import json
import struct
import argparse

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True, help='Locally supplied DLSS Enabler 4.9.0.15 version.dll')
source = parser.parse_args().source.resolve()
target = root / 'runtime/fg/dlss-enabler-headless.dll'
data = bytearray(source.read_bytes())
original = hashlib.sha256(data).hexdigest().upper()
assert original == 'F131734E9BF1FD191A9F60FAD54231CD88E45ADA3C957BD02C8763F8E9AD4440'
pe = struct.unpack_from('<I', data, 0x3c)[0]
assert data[pe:pe+4] == b'PE\0\0'
sections = struct.unpack_from('<H', data, pe+6)[0]
optional = struct.unpack_from('<H', data, pe+20)[0]
rva = 0x1be88
offset = None
for n in range(sections):
    header = pe+24+optional+n*40
    virtual_size, virtual_address, raw_size, raw = struct.unpack_from('<IIII', data, header+8)
    if virtual_address <= rva < virtual_address + raw_size:
        offset = raw + rva - virtual_address
        break
assert offset is not None
before = bytes.fromhex('85 d2 0f 84 ba 00 00 00')
# Jump directly to this callback's original success epilogue, AFTER its two
# module-handle assignments. Leave PE/CRT entry, constructors, TLS and all
# explicit DLSSG API functions untouched. This excludes proxy bootstrap/hooks
# and its unconditional TerminateProcess on DLL_PROCESS_DETACH.
after = b'\xe9' + struct.pack('<i', 0x1bff6 - (rva+5)) + b'\x90'*3
assert data[offset:offset+8] == before
data[offset:offset+8] = after
target.parent.mkdir(parents=True, exist_ok=True)
target.write_bytes(data)
result = hashlib.sha256(data).hexdigest().upper()
manifest = {
    'source': 'User supplied DLSS Enabler 4.9.0 archive, Nexus mod 757 file 9301',
    'version': '4.9.0.15', 'source_sha256': original, 'sha256': result,
    'adaptation': 'Local private API adapter; proxy callback bypass after module-handle setup. Original CRT entry and DLSSG APIs retained. Not an official headless distribution.',
    'patch': {'rva': hex(rva), 'before': before.hex(), 'after': after.hex()},
    'maximum_factor': 4, 'formats': ['SDR', 'HDR10/PQ'],
    'scope': 'Private addon runtime only; never install as a game proxy'
}
(target.parent/'enabler-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
print(result)
