"""Reader for the .nup game container (little endian, nothing encrypted).

    0x00  superblock, 0x80 bytes: "NUP\\x01", u16 0x80, u16 1, u64 toc offset, u64 toc size, sha256(toc),
          u64 blob offset, u64 file size, u32 package count, u32 0, 16-byte unique id, 32 zero bytes
    TOC:  u32 "TOC\\x01", u32 packages, u64 dirs, u64 files, u64 name pool offset (from the TOC), u64 pool length, u64 0
          packages, 0x40 each: u64 title id, u32 root dir, u32 exefs dir, u32 romfs dir, u32 version, u8 kind
                               (0 base, 1 update, 2 dlc), u8 flags, u16, u32, u64 meta offset, u32 meta length, 20 bytes
          dirs,     0x18 each: u32 name offset, u32 name length, u32 first child, u32 child count, u64
          files,    0x50 each: u32 name offset, u32 name length, u64 size, u64 data offset (absolute), u64 stored size,
                               sha256 of the file's bytes, u8 codec (0 stored, 1 one zstd frame), padding
          children: u32 each, a directory below the dir count, else dir count + file index
          names:    the pool

Each thread reads through its own handle: seeks on one shared handle race. Standard library plus zstandard.
"""
import hashlib
import struct
import threading

SUPER = struct.Struct('<4sHHQQ32sQQII16s32s')
PACKAGE = struct.Struct('<QIIIIBBHIQI20s')
DIRECTORY = struct.Struct('<IIIIQ')
FILE = struct.Struct('<IIQQQ32sBBH12s')
CHUNK = 8 << 20


class NupError(Exception):
    pass


class Nup:
    def __init__(self, path):
        self.path = str(path)
        self._local = threading.local()
        with open(self.path, 'rb') as f:
            head = f.read(SUPER.size)
            if len(head) < SUPER.size:
                raise NupError('not a .nup file (too short)')
            magic, _a, _b, toc_off, toc_size, toc_sha, _blob, _size, _count, _z, _uid, _zeros = SUPER.unpack(head)
            if magic != b'NUP\x01':
                raise NupError(f'not a .nup file (magic {magic!r})')
            f.seek(toc_off)
            toc = f.read(toc_size)
        if len(toc) != toc_size or hashlib.sha256(toc).digest() != toc_sha:
            raise NupError('the table of contents is damaged (its sha256 does not match)')
        # The sha256 only says the table is what the file's writer wrote: counts and offsets that do not fit it end in
        # a NupError, which the callers report, not in struct.error or IndexError
        try:
            self._parse(toc)
        except (struct.error, IndexError, ValueError, OverflowError, MemoryError) as e:
            raise NupError(f'the table of contents is malformed ({type(e).__name__}: {e})') from None

    def _parse(self, toc):
        tmagic, npkg, ndir, nfile, pool_off, pool_len, _ = struct.unpack_from('<IIQQQQQ', toc, 0)
        if tmagic != 0x01434F54:
            raise NupError('unknown table of contents')
        if (npkg * PACKAGE.size + ndir * DIRECTORY.size + nfile * FILE.size + 0x30 > len(toc)
                or pool_off + pool_len > len(toc)):
            raise NupError('the table of contents is malformed (its counts or name pool run past its end)')
        at = 0x30
        self.packages = []
        for i in range(npkg):
            tid, root, exefs, romfs, version, kind, *_ = PACKAGE.unpack_from(toc, at + i * PACKAGE.size)
            self.packages.append(dict(title_id=tid, root=root, exefs=exefs, romfs=romfs, version=version, kind=kind))
        at += npkg * PACKAGE.size
        self.dirs = [DIRECTORY.unpack_from(toc, at + i * DIRECTORY.size) for i in range(ndir)]
        at += ndir * DIRECTORY.size
        self.files = [FILE.unpack_from(toc, at + i * FILE.size) for i in range(nfile)]
        at += nfile * FILE.size
        if pool_off < at:
            raise NupError('the table of contents is malformed (its name pool overlaps its entries)')
        self.children = struct.unpack_from(f'<{(pool_off - at) // 4}I', toc, at)
        self.pool = toc[pool_off:pool_off + pool_len]
        self.ndir = ndir

    def _name(self, off, n):
        try:
            return self.pool[off:off + n].decode('utf-8')
        except UnicodeDecodeError:
            raise NupError(f'a name in the table of contents is not UTF-8 (at {off:#x})') from None

    def tree(self, dir_index):
        """(path relative to the directory, file index) for every file below it. A directory met twice (one listed
        as its own child or ancestor) is a NupError, where it went round for good."""
        stack = [(dir_index, '')]
        seen = set()
        while stack:
            d, prefix = stack.pop()
            if d in seen:
                raise NupError(f'directory {d} of the table of contents is listed below itself')
            seen.add(d)
            try:
                _no, _nl, first, count, _ = self.dirs[d]
                entries = self.children[first:first + count]
                for c in entries:
                    if c < self.ndir:
                        no, nl = self.dirs[c][0], self.dirs[c][1]
                        stack.append((c, prefix + self._name(no, nl) + '/'))
                    else:
                        fi = c - self.ndir
                        yield prefix + self._name(self.files[fi][0], self.files[fi][1]), fi
            except IndexError:
                raise NupError(f'directory {d} of the table of contents names an entry it does not have') from None

    def size(self, fi):
        return self.files[fi][2]

    def _handle(self):
        f = getattr(self._local, 'f', None)
        if f is None:
            f = self._local.f = open(self.path, 'rb')
        return f

    def chunks(self, fi):
        """The file's bytes in pieces, checked against its sha256 at the end (NupError when they differ)."""
        _no, _nl, size, off, stored, sha, codec, *_ = self.files[fi]
        f = self._handle()
        f.seek(off)
        h = hashlib.sha256()
        got = 0
        if codec == 0:
            left = stored
            while left:
                b = f.read(min(CHUNK, left))
                if not b:
                    break
                left -= len(b)
                got += len(b)
                h.update(b)
                yield b
        elif codec == 1:
            import zstandard
            d = zstandard.ZstdDecompressor().decompressobj()
            left = stored
            while left:
                b = f.read(min(CHUNK, left))
                if not b:
                    break
                left -= len(b)
                try:
                    out = d.decompress(b)
                except zstandard.ZstdError as e:
                    raise NupError(f'file {fi}: damaged ({e})') from None
                if out:
                    got += len(out)
                    h.update(out)
                    yield out
        else:
            raise NupError(f'file {fi}: unknown codec {codec}')
        if got != size or h.digest() != sha:
            raise NupError(f'file {fi}: damaged (size or sha256 differs from the table of contents)')

    def read(self, fi):
        return b''.join(self.chunks(fi))
