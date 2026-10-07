# GGUF fixtures for the tests of the Provision admin layer (fork-owned, next to utils.py so that utils.py stays as
# upstream has it). Pure Python, no gguf-py and no numpy: every fixture is derived from a small model of the shared
# test cache (stories260K) or written from scratch, and big files are streamed to disk, never held in memory.
#
#   parse / info           read the metadata and the tensor infos of a GGUF file
#   copy                   a copy with some metadata values replaced, added or removed
#   split                  a split set in the layout of llama-gguf-split (<prefix>-NNNNN-of-NNNNN.gguf)
#   projector              a minimal projector (general.architecture = clip) without tensors
#   big_copy               a valid GGUF of any size: the model plus one large tensor of zeros, streamed
#   crc32_hex / sha256_*   the identities the upload protocol uses

import hashlib
import os
import struct
import zlib

# GGUF value types
T_UINT8, T_INT8, T_UINT16, T_INT16, T_UINT32, T_INT32, T_FLOAT32, T_BOOL, T_STRING, T_ARRAY, T_UINT64, T_INT64, T_FLOAT64 = range(13)

_SCALARS = {T_UINT8: "<B", T_INT8: "<b", T_UINT16: "<H", T_INT16: "<h", T_UINT32: "<I", T_INT32: "<i", T_FLOAT32: "<f",
            T_BOOL: "<?", T_UINT64: "<Q", T_INT64: "<q", T_FLOAT64: "<d"}

MiB = 1024 * 1024


def parse(data: bytes) -> dict:
    """the header of a GGUF file: version, metadata (decoded and raw), tensor infos, alignment, the data section"""
    pos = 0

    def take(n: int) -> bytes:
        nonlocal pos
        out = data[pos:pos + n]
        if len(out) != n:
            raise ValueError("the file ends inside its GGUF header")
        pos += n
        return out

    def u32() -> int:
        return struct.unpack("<I", take(4))[0]

    def u64() -> int:
        return struct.unpack("<Q", take(8))[0]

    def gstr() -> str:
        return take(u64()).decode("utf-8", errors="replace")

    def value(t: int):
        if t == T_STRING:
            return gstr()
        if t == T_ARRAY:
            et, n = u32(), u64()
            return [value(et) for _ in range(n)]
        fmt = _SCALARS[t]
        return struct.unpack(fmt, take(struct.calcsize(fmt)))[0]

    if take(4) != b"GGUF":
        raise ValueError("not a GGUF file")
    version = u32()
    n_tensors, n_kv = u64(), u64()
    kv: dict = {}
    kv_raw: list = []
    for _ in range(n_kv):
        start = pos
        key = gstr()
        kv[key] = value(u32())
        kv_raw.append((key, data[start:pos]))
    tensors: list = []
    for _ in range(n_tensors):
        start = pos
        name = gstr()
        dims = [u64() for _ in range(u32())]
        ggml_type = u32()
        info = data[start:pos]  # everything of the tensor info but its offset
        tensors.append({"name": name, "dims": dims, "type": ggml_type, "offset": u64(), "info": info})
    alignment = int(kv.get("general.alignment", 32))
    data_start = (pos + alignment - 1) // alignment * alignment
    return {"version": version, "kv": kv, "kv_raw": kv_raw, "tensors": tensors, "alignment": alignment,
            "header_end": data_start, "data": data[data_start:]}


def info(path) -> dict:
    with open(path, "rb") as f:
        return parse(f.read())


#
# encoding
#


def enc_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<Q", len(b)) + b


def val(t: int, v) -> bytes:
    """a typed metadata value (type + payload), for copy(values=...)"""
    if t == T_STRING:
        return struct.pack("<I", T_STRING) + enc_str(v)
    return struct.pack("<I", t) + struct.pack(_SCALARS[t], v)


def val_str_array(items: list) -> bytes:
    return struct.pack("<IIQ", T_ARRAY, T_STRING, len(items)) + b"".join(enc_str(s) for s in items)


def kv(key: str, typed_value: bytes) -> bytes:
    """one raw metadata entry: key + typed value (see val)"""
    return enc_str(key) + typed_value


def _tensor_regions(g: dict) -> list:
    """(tensor, bytes of its data including the padding up to the next tensor), in file order"""
    ordered = sorted(g["tensors"], key=lambda t: t["offset"])
    out = []
    for i, t in enumerate(ordered):
        end = ordered[i + 1]["offset"] if i + 1 < len(ordered) else len(g["data"])
        out.append((t, g["data"][t["offset"]:end]))
    return out


def _pad(n: int, alignment: int) -> int:
    return (-n) % alignment


def write(dst, version: int, kvs: list, tensors: list, alignment: int = 32) -> str:
    """a GGUF file from raw metadata entries and (tensor info without offset, data) pairs, offsets recomputed"""
    infos = []
    blob = bytearray()
    for tinfo, tdata in tensors:
        blob += b"\0" * _pad(len(blob), alignment)
        infos.append(tinfo + struct.pack("<Q", len(blob)))
        blob += tdata
    blob += b"\0" * _pad(len(blob), alignment)
    head = b"GGUF" + struct.pack("<IQQ", version, len(infos), len(kvs)) + b"".join(kvs) + b"".join(infos)
    head += b"\0" * _pad(len(head), alignment)
    with open(dst, "wb") as f:
        f.write(head)
        f.write(bytes(blob))
    return str(dst)


def copy(src, dst, strings: dict | None = None, values: dict | None = None, drop_keys=(), extra_kvs=(),
         drop_tensors=(), truncate: int = 0) -> str:
    """a copy of a GGUF file with string values (or typed values, see val) replaced or added, keys removed, raw entries
    appended, tensors left out, and `truncate` bytes cut off the end"""
    g = info(src)
    strings = dict(strings or {})
    values = dict(values or {})
    kvs = []
    for key, raw in g["kv_raw"]:
        if key in drop_keys:
            continue
        if key in strings:
            kvs.append(kv(key, val(T_STRING, strings.pop(key))))
        elif key in values:
            kvs.append(kv(key, values.pop(key)))
        else:
            kvs.append(raw)
    for key, text in strings.items():
        kvs.append(kv(key, val(T_STRING, text)))
    for key, typed in values.items():
        kvs.append(kv(key, typed))
    kvs.extend(extra_kvs)
    tensors = [(t["info"], data) for t, data in _tensor_regions(g) if t["name"] not in drop_tensors]
    path = write(dst, g["version"], kvs, tensors, g["alignment"])
    if truncate:
        with open(path, "r+b") as f:
            f.truncate(os.path.getsize(path) - truncate)
    return path


def split(src, dst_dir, prefix: str, n_split: int) -> list:
    """the model split into n_split files the way llama-gguf-split writes them: the first file holds all metadata,
    every file holds split.no (u16), split.count (u16) and split.tensors.count (i32), the tensors are spread in order"""
    g = info(src)
    regions = _tensor_regions(g)
    n_tensors = len(regions)
    if n_split < 2 or n_split > n_tensors:
        raise ValueError(f"cannot split {n_tensors} tensors into {n_split} files")
    base = [raw for key, raw in g["kv_raw"] if not key.startswith("split.")]
    per = (n_tensors + n_split - 1) // n_split
    os.makedirs(dst_dir, exist_ok=True)
    paths = []
    for i in range(n_split):
        part = regions[i * per:(i + 1) * per] if i < n_split - 1 else regions[i * per:]
        split_kvs = [kv("split.no", val(T_UINT16, i)), kv("split.count", val(T_UINT16, n_split)),
                     kv("split.tensors.count", val(T_INT32, n_tensors))]
        kvs = (base if i == 0 else []) + split_kvs
        path = os.path.join(str(dst_dir), "%s-%05d-of-%05d.gguf" % (prefix, i + 1, n_split))
        paths.append(write(path, g["version"], kvs, [(t["info"], data) for t, data in part], g["alignment"]))
    return paths


def projector(dst, vision: bool = True, audio: bool = False, name: str = "test projector", typed: dict | None = None,
              extra_kvs=()) -> str:
    """a projector without tensors: general.architecture = clip and the keys the router's capability probe reads.
    typed: {key: raw typed value (see val)} replaces or adds one of those keys with any type"""
    entries = {
        "general.architecture": val(T_STRING, "clip"),
        "general.name": val(T_STRING, name),
        "general.description": val(T_STRING, "a projector written by the admin tests"),
        "clip.has_vision_encoder": val(T_BOOL, vision),
        "clip.has_audio_encoder": val(T_BOOL, audio),
    }
    entries.update(typed or {})
    kvs = [kv(key, typed_value) for key, typed_value in entries.items()] + list(extra_kvs)
    return write(dst, 3, kvs, [], 32)


def big_copy(src, dst, extra_bytes: int) -> str:
    """the model plus one F32 tensor of extra_bytes zeros at the end (a valid GGUF of about that size), written in a
    stream; returns the SHA-256 of the file"""
    g = info(src)
    tensors = [(t["info"], data) for t, data in _tensor_regions(g)]
    n = extra_bytes // 4
    pad_info = enc_str("provision_test_pad.weight") + struct.pack("<I", 1) + struct.pack("<Q", n) + struct.pack("<I", 0)
    infos = []
    blob = bytearray()
    for tinfo, tdata in tensors:
        blob += b"\0" * _pad(len(blob), g["alignment"])
        infos.append(tinfo + struct.pack("<Q", len(blob)))
        blob += tdata
    blob += b"\0" * _pad(len(blob), g["alignment"])
    infos.append(pad_info + struct.pack("<Q", len(blob)))
    kvs = [raw for _, raw in g["kv_raw"]]
    head = b"GGUF" + struct.pack("<IQQ", g["version"], len(infos), len(kvs)) + b"".join(kvs) + b"".join(infos)
    head += b"\0" * _pad(len(head), g["alignment"])
    h = hashlib.sha256()
    zero = b"\0" * (16 * MiB)
    with open(dst, "wb") as f:
        for part in (head, bytes(blob)):
            f.write(part)
            h.update(part)
        left = n * 4
        while left > 0:
            k = min(left, len(zero))
            f.write(zero[:k])
            h.update(zero[:k])
            left -= k
    return h.hexdigest()


#
# identities
#


def crc32_hex(data: bytes) -> str:
    return "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)


def head_crc32(path) -> str:
    """CRC32 of the first min(1 MiB, size) bytes: the resume identity of a file in the upload protocol"""
    with open(path, "rb") as f:
        return crc32_hex(f.read(MiB))


def sha256_file(path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(4 * MiB)
            if not b:
                break
            h.update(b)
    return h.hexdigest()
