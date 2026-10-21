#!/usr/bin/env python3
"""Import a Tears of the Kingdom dump into the extracted layout the runtime reads (Windows, macOS, Linux).

Input: the base game (XCI or NSP), an optional cumulative update NSP, and the player's own prod.keys.
Output (the same layout build_app.py writes under work/totk/extracted, so the later steps can use it):

  <out>/extracted/v0/exefs/            base ExeFS (main, main.npdm, rtld, sdk, subsdk0), hash-verified per block
  <out>/extracted/v0/elf/              the NSOs decompressed (segment hashes verified) as analysis ELF + inventories
  <out>/extracted/<ver>/exefs, elf/    the same for the update; <ver> (v112/v121/v143) is chosen by module hashes
  <out>/extracted/<ver>/romfs/         romfs.bin (base + update layered: BKTR merged here, in Python) + index.json
  <out>/extracted/v0/romfs/            base RomFS: index.json only by default (read lazily from the NSP),
                                       or the full romfs.bin with --base-romfs full (needed only without an update)
  <out>/import-manifest.json           what was found and verified

Every step reuses the repository's own readers (games/totk/scripts/extract_exefs.py, extract_romfs.py,
prepare_modules.py, and the version tables of build_app.py). Only the BKTR merge, which build_app.py does with a
patched hactool build, is reimplemented here so no C toolchain is needed; it verifies all six IVFC levels of the
merged image and the image's SHA-256 against games/totk/assets/expected-<version>.json.

Keys and game data stay where they are and in <out>; nothing is written into the repository.
Needs: Python 3.8+, `pip install cryptography lz4`.
"""
import argparse
import bisect
import concurrent.futures
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import threading
import time
from collections import deque
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / 'games/totk/scripts'
sys.path[:0] = [str(REPO), str(SCRIPTS)]

try:
    from cryptography.hazmat.primitives.ciphers import modes  # noqa: F401  (extract_exefs needs it too)
    import lz4.block  # noqa: F401  (prepare_modules needs it)
except ImportError as missing:
    sys.exit(f'missing Python package ({missing.name}): {sys.executable} -m pip install cryptography lz4')

import build_app  # noqa: E402  (version tables and ExeFS checks; importing it runs nothing platform-specific)
from build_app import VERSIONS, UPDATES, BuildError, exefs_matches, npdm_kind, sha256, source_matches, say  # noqa: E402
from extract_exefs import (decrypt, read_at, require, load_keys, container_entries, common_tickets,  # noqa: E402
                           nca_inventory, program_nca, nca_ctr_key, record_offset, partition_entries)
from extract_romfs import index_romfs  # noqa: E402

CHUNK = 8 << 20
NODE = 0x4000   # bucket-tree node size


# ---- NCA sections ----
class CtrSection:
    """One AES-CTR NCA section, read in place from the container."""

    def __init__(self, path, record, hdr, index, key):
        start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * index)
        require(start, f'NCA section {index} is empty')
        self.fs = hdr[0x400 + 512 * index:0x600 + 512 * index]
        require(hashlib.sha256(self.fs).digest() == hdr[0x280 + 32 * index:0x2A0 + 32 * index], 'FS header hash mismatch')
        self.path, self.nca, self.key, self.local = path, record_offset(record), key, threading.local()
        self.start, self.size = start * 512, (end - start) * 512
        self.upper = self.fs[0x140:0x148][::-1]   # big-endian upper counter: secure value, generation

    def read(self, relative, size, upper=None, encrypted=True):
        require(0 <= relative and relative + size <= self.size, 'NCA section read out of bounds')
        absolute = self.start + relative
        aligned = absolute & ~15
        f = getattr(self.local, 'f', None)
        if f is None:   # one handle per thread: seek + read on a shared handle would race
            f = self.local.f = open(self.path, 'rb')
        data = read_at(f, self.nca + aligned, size + absolute - aligned)
        if encrypted:
            iv = (upper or self.upper) + (aligned // 16).to_bytes(8, 'big')
            data = decrypt(self.key, modes.CTR(iv), data)
        return data[absolute - aligned:]

    def ivfc_levels(self):
        require(self.fs[8:12] == b'IVFC' and struct.unpack_from('<III', self.fs, 12) == (0x20000, 32, 7), 'Unsupported IVFC layout')
        return [struct.unpack_from('<QQII', self.fs, 0x18 + j * 24) for j in range(6)]


def bucket_tree(table, entry_size, fmt, count):
    """A two-level Nintendo bucket tree (one L1 node of entry-set offsets, then entry-set nodes)."""
    _, sets, total = struct.unpack_from('<iiq', table, 0)
    require(0 < sets <= (NODE - 16) // 8 and len(table) >= NODE * (1 + sets), 'Unsupported BKTR table shape')
    entries = []
    for s in range(sets):
        node = NODE * (1 + s)
        _, n, _end = struct.unpack_from('<iiq', table, node)
        require(0 < n <= (NODE - 16) // entry_size, 'Invalid BKTR entry set')
        entries += [struct.unpack_from(fmt, table, node + 16 + k * entry_size) for k in range(n)]
    require(len(entries) == count, 'BKTR entry count differs from its header')
    require(all(a[0] < b[0] for a, b in zip(entries, entries[1:])) and entries[0][0] == 0, 'BKTR entries out of order')
    return entries, total


class PatchedSection:
    """The update's virtual RomFS section: indirect storage over base (0) and patch (1, AES-CTR-EX) storages."""

    def __init__(self, base, patch):
        fs = patch.fs
        require(fs[4] == 4, 'Expected an AES-CTR-EX (BKTR) update section')
        roff, rsize, rmagic, _, rcount, _ = struct.unpack_from('<QQ4sIII', fs, 0x100)
        soff, ssize, smagic, _, scount, _ = struct.unpack_from('<QQ4sIII', fs, 0x120)
        require(rmagic == smagic == b'BKTR' and roff + rsize == soff and soff + ssize <= patch.size
                and (patch.size - soff - ssize) % 0x200 == 0, 'Unsupported BKTR layout')
        # The tables are encrypted with the section's own counter (plain AES-CTR), like the rest of a normal section.
        self.reloc, self.size = bucket_tree(patch.read(roff, rsize), 20, '<qqi', rcount)
        self.subs, subs_end = bucket_tree(patch.read(soff, ssize), 16, '<qBxxxI', scount)
        require(subs_end == roff, 'BKTR subsection table does not end at the relocation table')
        require(all(e[2] in (0, 1) for e in self.reloc) and all(e[1] in (0, 1) for e in self.subs), 'Unknown BKTR storage')
        self.reloc_starts = [e[0] for e in self.reloc]
        self.subs_starts = [e[0] for e in self.subs]
        self.subs_end = subs_end
        self.base, self.patch = base, patch

    def _patch_read(self, offset, size):
        out = []
        while size:
            k = bisect.bisect_right(self.subs_starts, offset) - 1
            start, unencrypted, generation = self.subs[k]
            end = self.subs[k + 1][0] if k + 1 < len(self.subs) else self.subs_end
            require(start <= offset < end, 'Patch read outside the subsection table')
            n = min(size, end - offset)
            upper = self.patch.upper[:4] + generation.to_bytes(4, 'big')
            out.append(self.patch.read(offset, n, upper=upper, encrypted=not unencrypted))
            offset += n
            size -= n
        return b''.join(out)

    def read(self, offset, size):
        require(0 <= offset and offset + size <= self.size, 'Virtual RomFS read out of bounds')
        out = []
        while size:
            k = bisect.bisect_right(self.reloc_starts, offset) - 1
            virt, phys, storage = self.reloc[k]
            end = self.reloc[k + 1][0] if k + 1 < len(self.reloc) else self.size
            n = min(size, end - offset)
            source = phys + offset - virt
            out.append(self._patch_read(source, n) if storage else self.base.read(source, n))
            offset += n
            size -= n
        return b''.join(out)

    def ivfc_levels(self):
        return self.patch.ivfc_levels()

    @property
    def fs(self):
        return self.patch.fs


class LazyRomfs(io.RawIOBase):
    """The RomFS data level of a section as a read-only file, so index_romfs reads it without an image on disk."""

    def __init__(self, section):
        self.section = section
        self.offset, self.length = section.ivfc_levels()[5][:2]
        self.pos = 0

    # index_romfs takes a path: open() and stat() are the two calls it makes
    def open(self, mode='rb'):
        self.pos = 0
        return io.BufferedReader(self, 1 << 20)

    def stat(self):
        return SimpleNamespace(st_size=self.length)

    def readable(self):
        return True

    def seekable(self):
        return True

    def seek(self, pos, whence=0):
        self.pos = pos if whence == 0 else self.pos + pos if whence == 1 else self.length + pos
        return self.pos

    def tell(self):
        return self.pos

    def readinto(self, buffer):
        n = max(0, min(len(buffer), self.length - self.pos))
        if n:
            buffer[:n] = self.section.read(self.offset + self.pos, n)
            self.pos += n
        return n


def stream_romfs(section, image_path):
    """Verify all six IVFC levels of a section and write its data level to image_path (the repository's
    extract_romfs.py check, over any section reader). Returns (blocks per level, size, sha256)."""
    levels = section.ivfc_levels()
    parent, counts, digest = section.fs[0xC8:0xE8], [], hashlib.sha256()
    t0, shown, total = time.time(), time.time(), levels[5][1]

    def ahead(pool, offset, size, depth=6):   # read + decrypt ahead on worker threads, in order
        window = deque()
        for pos in range(0, size, CHUNK):
            window.append(pool.submit(section.read, offset + pos, min(CHUNK, size - pos)))
            if len(window) >= depth:
                yield window.popleft().result()
        while window:
            yield window.popleft().result()

    with open(image_path, 'wb') as image, concurrent.futures.ThreadPoolExecutor(4) as pool:
        for level, (offset, size, power, reserved) in enumerate(levels):
            require(12 <= power <= 24 and reserved == 0, 'IVFC block size')
            block = 1 << power
            require(len(parent) >= (size + block - 1) // block * 32, 'IVFC parent size')
            captured = bytearray() if level < 5 else None
            index = 0
            for chunk in ahead(pool, offset, size):
                for j in range(0, len(chunk), block):
                    payload = chunk[j:j + block]
                    if len(payload) < block:
                        payload += bytes(block - len(payload))
                    require(hashlib.sha256(payload).digest() == parent[index * 32:index * 32 + 32],
                            f'IVFC level {level} block {index} failed')
                    index += 1
                if captured is not None:
                    captured.extend(chunk)
                    continue
                image.write(chunk)
                digest.update(chunk)
                if time.time() - shown > 30:
                    shown, done = time.time(), min(index * block, total)
                    say(f'   RomFS {done / 2**30:.1f}/{total / 2**30:.1f} GiB ({done / (shown - t0) / 2**20:.0f} MiB/s)')
            counts.append(index)
            if captured is not None:
                parent = bytes(captured)
    return counts, levels[5][1], digest.hexdigest()


# ---- containers ----
class Package:
    """An open XCI/NSP: entries, tickets and NCA headers, read once."""

    def __init__(self, path, keys):
        self.path, self.keys = path, keys
        self.f = path.open('rb')
        self.kind, self.entries = container_entries(self.f, path.stat().st_size)
        self.tickets = common_tickets(self.f, self.entries)
        self.records = nca_inventory(self.f, self.entries, keys)

    def program(self, update=False):
        record, hdr = program_nca(self.records, update=update)
        return record, hdr, nca_ctr_key(hdr, self.keys, self.tickets)

    def romfs_section(self, update=False):
        record, hdr, key = self.program(update)
        for i in range(4):
            start = struct.unpack_from('<I', hdr, 0x240 + 16 * i)[0]
            fs = hdr[0x400 + 512 * i:0x600 + 512 * i]
            if start and fs[2] == 0 and fs[3] == 3 and fs[4] in ((4,) if update else (3,)):
                return CtrSection(self.path, record, hdr, i, key)
        raise BuildError(f'{self.path.name}: no {"BKTR " if update else ""}RomFS section in the Program NCA')

    def meta_version(self):
        """(title ID, title version) from the package's content meta (.cnmt), independent of the ExeFS hashes."""
        for record, hdr in self.records:
            if record['content_type'] != 1:
                continue
            section = CtrSection(self.path, record, hdr, 0, nca_ctr_key(hdr, self.keys, self.tickets))
            _, _, pfs_offset, pfs_size = struct.unpack_from('<QQQQ', section.fs, 0x30)
            pfs = section.read(pfs_offset, pfs_size)
            for name, offset, size in partition_entries(io.BytesIO(pfs), 0, len(pfs)):
                if name.endswith('.cnmt') and size >= 0x20:
                    title, version = struct.unpack_from('<QI', pfs, offset)
                    return f'{title:016X}', version
        return None, None

    def close(self):
        self.f.close()


# ---- steps ----
def run_script(script, *arguments):
    command = [sys.executable, str(SCRIPTS / script), *map(str, arguments)]
    p = subprocess.run(command, capture_output=True, text=True)
    if p.returncode:
        raise BuildError(f'{script} failed (exit {p.returncode}):\n' + (p.stdout + p.stderr)[-3000:])
    return p.stdout


def exefs_report(exefs, expected):
    """Per-file comparison with an expected-<version>.json ExeFS table."""
    rows = {}
    for name, want in expected['exefs_sha256'].items():
        got = sha256(exefs / name) if (exefs / name).is_file() else None
        kind = 'match' if got == want else (npdm_kind(exefs / name, expected['title_id']) if name == 'main.npdm' and got else None)
        rows[name] = dict(sha256=got, expected=want, result=kind or ('missing' if got is None else 'MISMATCH'))
    return rows


def step_exefs(out, source, keys, update):
    """Extract (or reuse) an ExeFS and choose its version by module hashes, as build_app.identify_update does."""
    candidates = UPDATES if update else ('v0',)
    for version in candidates:
        exefs = out / f'extracted/{version}/exefs'
        if source_matches(exefs.parent / 'extraction-manifest.json', source) and \
                exefs_matches(exefs, VERSIONS[version]['exefs_sha256'], VERSIONS[version]['title_id']):
            say(f'ExeFS {VERSIONS[version]["display_version"]}: already extracted and verified')
            return version
    staging = out / 'extracted/detection'
    if staging.exists():
        shutil.rmtree(staging)
    say(f'extracting the {"update" if update else "base"} ExeFS from {source.name}')
    run_script('extract_exefs.py', source, '--keys', keys, '--output', staging / 'exefs', *(['--update'] if update else []))
    version = next((v for v in candidates if exefs_matches(staging / 'exefs', VERSIONS[v]['exefs_sha256'], VERSIONS[v]['title_id'])), None)
    if version is None:
        report = {v: exefs_report(staging / 'exefs', VERSIONS[v]) for v in candidates}
        (staging / 'hash-report.json').write_text(json.dumps(report, indent=2) + '\n')
        raise BuildError(f'unsupported module hashes (expected {", ".join(VERSIONS[v]["display_version"] for v in candidates)}); '
                         f'the per-file comparison is in {staging / "hash-report.json"}')
    target = out / f'extracted/{version}'
    target.mkdir(parents=True, exist_ok=True)
    for name in ('exefs', 'elf'):
        if (target / name).exists():
            shutil.rmtree(target / name)
    (staging / 'exefs').rename(target / 'exefs')
    (staging / 'extraction-manifest.json').replace(target / 'extraction-manifest.json')
    staging.rmdir()
    say(f'ExeFS identified by verified module hashes: {VERSIONS[version]["display_version"]}')
    return version


def step_modules(out, version):
    elf = out / f'extracted/{version}/elf'
    if (elf / 'modules.json').is_file():
        return
    say(f'decompressing the {version} NSOs (rtld, main, sdk, subsdk0) and verifying their segment hashes')
    run_script('prepare_modules.py', out / f'extracted/{version}/exefs', '--output', elf)


def romfs_valid(folder, expected):
    try:
        report = json.loads((folder / 'index.json').read_text())
        return (report.get('status') == 'pass' and report['romfs_sha256'] == expected['romfs']['sha256']
                and (folder / 'romfs.bin').stat().st_size == expected['romfs']['size'])
    except (OSError, ValueError, KeyError):
        return False


def write_romfs(section, folder, expected, label):
    if romfs_valid(folder, expected):
        say(f'RomFS {label}: already extracted and verified')
        return json.loads((folder / 'index.json').read_text())
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    partial = folder / 'romfs.bin.partial'
    say(f'RomFS {label}: streaming {expected["romfs"]["size"] / 2**30:.1f} GiB and verifying all six IVFC levels')
    counts, size, digest = stream_romfs(section, partial)
    if digest != expected['romfs']['sha256'] or size != expected['romfs']['size']:
        raise BuildError(f'RomFS {label}: SHA-256 {digest[:16]}... ({size} bytes) is not the expected '
                         f'{expected["romfs"]["sha256"][:16]}... ({expected["romfs"]["size"]} bytes); the partial image is {partial}')
    partial.replace(folder / 'romfs.bin')
    files = index_romfs(folder / 'romfs.bin')
    report = dict(status='pass', ivfc_blocks=counts, romfs_size=size, romfs_sha256=digest, files=files,
                  integrity='all six IVFC levels verified; image SHA-256 matches the expected version')
    (folder / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def write_romfs_index(section, folder, label):
    """index.json of a RomFS read in place (no image). status 'index-only' so no image-reading step mistakes it."""
    index = folder / 'index.json'
    if index.is_file():
        try:
            if json.loads(index.read_text()).get('status') in ('index-only', 'pass'):
                return
        except ValueError:
            pass
    say(f'RomFS {label}: indexing in place (no image written)')
    folder.mkdir(parents=True, exist_ok=True)
    lazy = LazyRomfs(section)
    files = index_romfs(lazy)
    (index).write_text(json.dumps(dict(status='index-only', romfs_size=lazy.length, files=files,
                                       integrity='RomFS tables read in place; data blocks not verified (no image)'),
                                  indent=2) + '\n')


def free_gib(path):
    while not path.exists():
        path = path.parent
    return shutil.disk_usage(path).free / 2**30


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--base', required=True, type=Path, help='base game 1.0.0 (.xci or .nsp)')
    parser.add_argument('--update', type=Path, help='cumulative update (.nsp; .nsz: decompress it first with nsz -D)')
    parser.add_argument('--keys', required=True, type=Path, help="the player's prod.keys")
    parser.add_argument('--output', required=True, type=Path, help='data folder (outside the repository)')
    parser.add_argument('--romfs', choices=('full', 'index', 'none'), default='full',
                        help='the playable RomFS (the update one, else the base): full image (default), index only, or none')
    parser.add_argument('--base-romfs', choices=('full', 'index', 'none'), default='index',
                        help='base RomFS when an update is given (default: index only; the update image holds all data)')
    args = parser.parse_args(argv)
    base, keys, out = args.base.expanduser().resolve(), args.keys.expanduser().resolve(), args.output.expanduser().resolve()
    update = args.update.expanduser().resolve() if args.update else None
    for path in (base, keys, update):
        if path and not path.is_file():
            raise BuildError(f'no such file: {path}')
    if out == REPO or REPO in out.parents:
        raise BuildError('the output folder holds game data: choose one outside the repository')
    if build_app.input_kind(base) not in ('xci', 'nsp'):
        raise BuildError('the base game must be an uncompressed XCI or NSP')
    if update and build_app.input_kind(update) != 'nsp':
        raise BuildError('the update must be an uncompressed NSP (decompress an NSZ with: nsz -D --keys prod.keys file.nsz)')

    playable = 'v0'
    need = 1.0 + (17.5 if args.romfs == 'full' else 0) + (16.5 if update and args.base_romfs == 'full' else 0)
    have = free_gib(out)
    say(f'output {out}: {have:.0f} GiB free, this import needs up to {need:.0f} GiB')
    if have < need:
        raise BuildError(f'not enough free space: {need:.0f} GiB needed, {have:.0f} GiB free (try --romfs index)')
    out.mkdir(parents=True, exist_ok=True)
    keyset = load_keys(keys)
    t0 = time.time()
    manifest = dict(format=1, base=str(base), update=str(update) if update else None, versions={})

    base_pkg = Package(base, keyset)
    try:
        title, meta = base_pkg.meta_version()
        v0 = step_exefs(out, base, keys, update=False)
        step_modules(out, v0)
        manifest['versions'][v0] = dict(display=VERSIONS[v0]['display_version'], cnmt_title=title, cnmt_version=meta,
                                        exefs=exefs_report(out / f'extracted/{v0}/exefs', VERSIONS[v0]))
        base_section = base_pkg.romfs_section()
        base_mode = args.base_romfs if update else args.romfs
        if base_mode == 'full':
            write_romfs(base_section, out / 'extracted/v0/romfs', VERSIONS['v0'], '1.0.0')
        elif base_mode == 'index':
            write_romfs_index(base_section, out / 'extracted/v0/romfs', '1.0.0')
        if update:
            upd_pkg = Package(update, keyset)
            try:
                title, meta = upd_pkg.meta_version()
                playable = step_exefs(out, update, keys, update=True)
                expected = VERSIONS[playable]
                if meta is not None and meta != expected['version']:
                    raise BuildError(f'the update meta says version {meta}, its modules say {expected["display_version"]} '
                                     f'({expected["version"]})')
                step_modules(out, playable)
                manifest['versions'][playable] = dict(display=expected['display_version'], cnmt_title=title, cnmt_version=meta,
                                                      exefs=exefs_report(out / f'extracted/{playable}/exefs', expected))
                section = PatchedSection(base_section, upd_pkg.romfs_section(update=True))
                folder = out / f'extracted/{playable}/romfs'
                if args.romfs == 'full':
                    report = write_romfs(section, folder, expected, f'{expected["display_version"]} (base + update)')
                    manifest['versions'][playable]['romfs'] = dict(size=report['romfs_size'], sha256=report['romfs_sha256'],
                                                                   files=len(report['files']), matches_expected=True)
                elif args.romfs == 'index':
                    write_romfs_index(section, folder, f'{expected["display_version"]} (base + update)')
            finally:
                upd_pkg.close()
    finally:
        base_pkg.close()
    manifest['playable_version'] = playable
    (out / 'import-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    for version, info in manifest['versions'].items():
        say(f'{info["display"]}: ' + ', '.join(f'{n} {r["result"]}' for n, r in info['exefs'].items()))
    say(f'done in {(time.time() - t0) / 60:.1f} min; playable version {VERSIONS[playable]["display_version"]}: {out}')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except (BuildError, ValueError) as e:
        print(f'\nERROR: {e}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
