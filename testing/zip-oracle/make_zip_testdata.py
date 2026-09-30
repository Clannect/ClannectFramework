#!/usr/bin/env python3
"""Builds modules/cfw-io/testdata/zip: archives written by Python's zipfile
and Info-ZIP's zip (the references), and expected.json with what each holds
(name, size, CRC-32, SHA-256) or the error a reader must report.

    python3 testing/zip-oracle/make_zip_testdata.py

The contents are generated from a fixed seed, so the script is repeatable.
"""
import hashlib
import io
import json
import os
import random
import shutil
import subprocess
import tempfile
import zipfile
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'modules', 'cfw-io', 'testdata', 'zip')
FIXED = (2020, 1, 1, 0, 0, 0)

rng = random.Random(1234)


def text(n):
    words = ['clannect', 'framework', 'zip', 'deflate', 'window', 'glyph', 'socket', '\n']
    return ' '.join(rng.choice(words) for _ in range(n)).encode()


def noise(n):
    return bytes(rng.getrandbits(8) for _ in range(n))


FILES = {
    'README.md': text(400),
    'lib/libcfw-core.a': noise(3000) + text(3000),
    'include/cfw/core/String.h': text(200),
    'empty.txt': b'',
    'bin/random.bin': noise(20000),
}


def describe(path):
    """What a correct reader sees, from Python's zipfile."""
    entries = []
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            data = z.read(info)
            entries.append({'name': info.filename, 'size': len(data), 'crc32': zlib.crc32(data),
                            'sha256': hashlib.sha256(data).hexdigest()})
    return {'entries': entries}


def write(name, files, compression=zipfile.ZIP_DEFLATED, level=None, dirs=(), comment=b'', stream=False,
          force_zip64=False):
    path = os.path.join(OUT, name)
    target = open(path, 'wb')
    # A non-seekable file makes zipfile write data descriptors (flag bit 3).
    sink = _Unseekable(target) if stream else target
    with zipfile.ZipFile(sink, 'w', compression=compression, compresslevel=level) as z:
        for d in dirs:
            z.writestr(zipfile.ZipInfo(d, FIXED), b'')
        for n, data in files.items():
            info = zipfile.ZipInfo(n, FIXED)
            info.compress_type = compression
            if force_zip64:
                with z.open(info, 'w', force_zip64=True) as f:
                    f.write(data)
            else:
                z.writestr(info, data, compress_type=compression, compresslevel=level)
        z.comment = comment
    target.close()
    return path


class _Unseekable(io.RawIOBase):
    def __init__(self, f):
        self.f = f
        self.pos = 0

    def writable(self):
        return True

    def write(self, b):
        self.f.write(b)
        self.pos += len(b)
        return len(b)

    def tell(self):
        return self.pos

    def flush(self):
        self.f.flush()


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    expected = {}

    for name, kw in {
        'stored.zip': dict(compression=zipfile.ZIP_STORED),
        'deflated.zip': dict(),
        'deflated-level1.zip': dict(level=1),
        'deflated-level9.zip': dict(level=9),
        'descriptors.zip': dict(stream=True),
        'directories.zip': dict(dirs=('pkg/', 'pkg/empty/')),
        'comment.zip': dict(comment=b'an archive comment, like the ones installers carry'),
    }.items():
        expected[name] = describe(write(name, FILES, **kw))

    expected['utf8-names.zip'] = describe(write('utf8-names.zip', {
        'dokument/r\u00e4ksm\u00f6rg\u00e5s.txt': text(50), '\u65e5\u672c/\u30d5\u30a1\u30a4\u30eb.txt': text(30)}))
    expected['empty.zip'] = describe(write('empty.zip', {}))

    # Info-ZIP, as release packages are made.
    with tempfile.TemporaryDirectory() as tmp:
        tree = os.path.join(tmp, 'ClannectFramework-0.0.0-test')
        for n, data in FILES.items():
            p = os.path.join(tree, n)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'wb') as f:
                f.write(data)
            os.utime(p, (1577836800, 1577836800))
        path = os.path.join(OUT, 'infozip.zip')
        subprocess.run(['zip', '-qrX', path, os.path.basename(tree)], cwd=tmp, check=True)
    expected['infozip.zip'] = describe(path)

    # An archive after other bytes, as in an offline installer.
    with open(os.path.join(OUT, 'deflated.zip'), 'rb') as f:
        archive = f.read()
    with open(os.path.join(OUT, 'prefixed.bin'), 'wb') as f:
        f.write(b'MZ' + noise(5000) + archive)
    expected['prefixed.bin'] = expected['deflated.zip']

    # zip64 fields in the local headers only (sizes 0xFFFFFFFF there): still
    # readable from the central directory without zip64 support.
    expected['zip64-local.zip'] = describe(write('zip64-local.zip', {'a.txt': text(20)}, force_zip64=True))

    # Names that would escape the destination: they open, but extracting fails.
    evil = {}
    for n in ['../escaped.txt', '/absolute.txt', 'C:/drive.txt', 'dir\\backslash.txt', 'ok/../../up.txt']:
        write('evil.zip', {n: b'x'})
        evil[n] = open(os.path.join(OUT, 'evil.zip'), 'rb').read()
    os.remove(os.path.join(OUT, 'evil.zip'))
    for i, (n, data) in enumerate(evil.items()):
        with open(os.path.join(OUT, f'unsafe-{i}.zip'), 'wb') as f:
            f.write(data)
        expected[f'unsafe-{i}.zip'] = {'unsafe': n}

    with open(os.path.join(OUT, 'expected.json'), 'w') as f:
        json.dump(expected, f, indent=1, ensure_ascii=False)
        f.write('\n')
    print(len(expected), 'archives')


if __name__ == '__main__':
    main()
