#!/usr/bin/env python3
"""Writes a tiny random llama2.c-format model and tokenizer for testing Carbon:
grouped-query attention (4 query heads, 2 key/value heads), an unshared
classifier, and a vocabulary with byte fallback tokens and merges.

  make_tiny.py <out-dir>      -> <out-dir>/tiny.bin, <out-dir>/tiny_tok.bin
"""
import struct
import sys
from pathlib import Path

import numpy as np

out = Path(sys.argv[1])
rng = np.random.default_rng(1234)

pieces = [b"<unk>", b"\n<s>\n", b"\n</s>\n"] + [b"<0x%02X>" % b for b in range(256)]
words = [b" ", b"a", b"b", b"c", b"d", b"e", b"h", b"o", b"t", b"n", b"s", b"\n", b".",
         b" t", b"th", b"he", b"the", b" the", b"an", b" an", b"cat", b" cat", b"ca", b"at", b"on", b" on",
         b"\xc3\xa9", b"caf", b"caf\xc3\xa9"]
pieces += words
scores = [0.0] * 259 + [float(-i) for i in range(len(words))]
vocab = len(pieces)

with open(out / "tiny_tok.bin", "wb") as f:
    f.write(struct.pack("i", max(len(p) for p in pieces)))
    for p, s in zip(pieces, scores):
        f.write(struct.pack("fi", s, len(p)) + p)

dim, hidden, layers, heads, kv_heads, seq = 16, 40, 2, 4, 2, 48
kv_dim = dim * kv_heads // heads
head = dim // heads
shapes = [(vocab, dim), (layers, dim), (layers, dim, dim), (layers, kv_dim, dim), (layers, kv_dim, dim),
          (layers, dim, dim), (layers, dim), (layers, hidden, dim), (layers, dim, hidden), (layers, hidden, dim),
          (dim,), (seq * head,), (vocab, dim)]
with open(out / "tiny.bin", "wb") as f:
    f.write(struct.pack("7i", dim, hidden, layers, heads, kv_heads, -vocab, seq))  # negative: own classifier
    for i, shape in enumerate(shapes):
        scale = 1.0 if len(shape) <= 2 and i in (1, 6, 10) else 0.5
        a = rng.normal(0.0, scale, size=shape).astype(np.float32)
        if i in (1, 6, 10):
            a = 1.0 + 0.1 * a  # norm weights near 1
        f.write(a.tobytes())
