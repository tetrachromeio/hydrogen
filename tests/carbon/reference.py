#!/usr/bin/env python3
"""An independent numpy implementation of the llama2.c model, tokenizer and
sampler, to check Carbon against.

  reference.py <model.bin> <tokenizer.bin> <prompt> <steps> <temperature> <topp> <seed>

Prints the generated text, like Carbon's generate example. Arithmetic is float32
throughout, but numpy sums in a different order than Carbon, so logits agree to
about 1e-5, not bit for bit.
"""
import struct
import sys

import numpy as np


def load_model(path):
    raw = open(path, "rb").read()
    dim, hidden, n_layers, n_heads, n_kv, vocab, seq_len = struct.unpack("7i", raw[:28])
    shared = vocab > 0
    vocab = abs(vocab)
    w = np.frombuffer(raw, dtype=np.float32, offset=28)
    head = dim // n_heads
    kv_dim = dim * n_kv // n_heads
    at = 0

    def take(*shape):
        nonlocal at
        n = int(np.prod(shape))
        out = w[at:at + n].reshape(shape)
        at += n
        return out

    m = dict(dim=dim, hidden=hidden, L=n_layers, H=n_heads, KV=n_kv, vocab=vocab, seq=seq_len,
             head=head, kv_dim=kv_dim)
    m["embed"] = take(vocab, dim)
    m["rms_att"] = take(n_layers, dim)
    m["wq"] = take(n_layers, dim, dim)
    m["wk"] = take(n_layers, kv_dim, dim)
    m["wv"] = take(n_layers, kv_dim, dim)
    m["wo"] = take(n_layers, dim, dim)
    m["rms_ffn"] = take(n_layers, dim)
    m["w1"] = take(n_layers, hidden, dim)
    m["w2"] = take(n_layers, dim, hidden)
    m["w3"] = take(n_layers, hidden, dim)
    m["rms_final"] = take(dim)
    take(seq_len * head)  # RoPE tables, unused
    m["wcls"] = m["embed"] if shared else take(vocab, dim)
    m["kcache"] = np.zeros((n_layers, seq_len, kv_dim), np.float32)
    m["vcache"] = np.zeros((n_layers, seq_len, kv_dim), np.float32)
    return m


def rmsnorm(x, weight):
    ss = np.float32(np.mean(x * x, dtype=np.float32)) + np.float32(1e-5)
    return weight * (x / np.sqrt(ss)).astype(np.float32)


def softmax(x):
    e = np.exp(x - x.max())
    return (e / e.sum()).astype(np.float32)


def forward(m, token, pos):
    head, H, kv_dim = m["head"], m["H"], m["kv_dim"]
    kv_mul = H // m["KV"]
    x = m["embed"][token].copy()
    # RoPE angles for this position
    i = np.arange(0, m["dim"], 2)
    freq = (1.0 / np.power(np.float32(10000.0), (i % head).astype(np.float32) / np.float32(head))).astype(np.float32)
    ang = np.float32(pos) * freq
    cos, sin = np.cos(ang).astype(np.float32), np.sin(ang).astype(np.float32)

    def rope(v):
        v0, v1 = v[0::2].copy(), v[1::2].copy()
        n = len(v0)
        v[0::2] = v0 * cos[:n] - v1 * sin[:n]
        v[1::2] = v0 * sin[:n] + v1 * cos[:n]

    for l in range(m["L"]):
        xb = rmsnorm(x, m["rms_att"][l])
        q = m["wq"][l] @ xb
        k = m["wk"][l] @ xb
        v = m["wv"][l] @ xb
        rope(q)
        rope(k)
        m["kcache"][l, pos] = k
        m["vcache"][l, pos] = v
        out = np.zeros(m["dim"], np.float32)
        for h in range(H):
            qh = q[h * head:(h + 1) * head]
            off = (h // kv_mul) * head
            keys = m["kcache"][l, :pos + 1, off:off + head]
            att = softmax((keys @ qh) / np.sqrt(np.float32(head)))
            out[h * head:(h + 1) * head] = att @ m["vcache"][l, :pos + 1, off:off + head]
        x = x + m["wo"][l] @ out
        xb = rmsnorm(x, m["rms_ffn"][l])
        h1 = m["w1"][l] @ xb
        h3 = m["w3"][l] @ xb
        hb = (h1 * (np.float32(1.0) / (np.float32(1.0) + np.exp(-h1)))) * h3
        x = x + m["w2"][l] @ hb.astype(np.float32)
    x = rmsnorm(x, m["rms_final"])
    return m["wcls"] @ x


def load_tokenizer(path, vocab):
    raw = open(path, "rb").read()
    at = 4
    pieces, scores = [], []
    for _ in range(vocab):
        score, n = struct.unpack_from("fi", raw, at)
        at += 8
        pieces.append(raw[at:at + n])
        scores.append(score)
        at += n
    lookup = {}
    for i, p in enumerate(pieces):
        lookup.setdefault(p, i)
    return pieces, scores, lookup


def encode(tok, text):
    pieces, scores, lookup = tok
    data = text.encode()
    ids = [1]
    if data:
        ids.append(lookup[b" "])
    i = 0
    while i < len(data):
        j = i + 1
        while j < len(data) and (data[j] & 0xC0) == 0x80 and j - i < 4:
            j += 1
        chunk = data[i:j]
        if chunk in lookup:
            ids.append(lookup[chunk])
        else:
            ids.extend(b + 3 for b in chunk)
        i = j
    while True:
        best = (-1e10, -1, -1)
        for k in range(len(ids) - 1):
            merged = pieces[ids[k]] + pieces[ids[k + 1]]
            t = lookup.get(merged)
            if t is not None and scores[t] > best[0]:
                best = (scores[t], t, k)
        if best[1] < 0:
            break
        ids[best[2]:best[2] + 2] = [best[1]]
    return ids


def decode(tok, prev, token):
    p = tok[0][token]
    if prev == 1 and p.startswith(b" "):
        p = p[1:]
    if len(p) == 6 and p.startswith(b"<0x") and p.endswith(b">"):
        try:
            return bytes([int(p[3:5], 16)])
        except ValueError:
            pass
    return p


class Sampler:
    def __init__(self, temperature, topp, seed):
        self.t, self.p, self.rng = np.float32(temperature), np.float32(topp), seed

    def u32(self):
        M = (1 << 64) - 1
        self.rng ^= self.rng >> 12
        self.rng ^= (self.rng << 25) & M
        self.rng ^= self.rng >> 27
        return ((self.rng * 0x2545F4914F6CDD1D) & M) >> 32

    def sample(self, logits):
        if self.t == 0:
            return int(np.argmax(logits))
        probs = softmax((logits / self.t).astype(np.float32))
        coin = np.float32(self.u32() >> 8) / np.float32(16777216.0)
        if self.p <= 0 or self.p >= 1:
            cdf = np.cumsum(probs, dtype=np.float32)
            hit = np.nonzero(coin < cdf)[0]
            return int(hit[0]) if len(hit) else len(probs) - 1
        cutoff = (np.float32(1.0) - self.p) / np.float32(len(probs) - 1)
        idx = np.nonzero(probs >= cutoff)[0]
        idx = idx[np.argsort(-probs[idx], kind="stable")]
        cum, last = np.float32(0), len(idx) - 1
        for k, t in enumerate(idx):
            cum += probs[t]
            if cum > self.p:
                last = k
                break
        r, cdf = coin * cum, np.float32(0)
        for t in idx[:last + 1]:
            cdf += probs[t]
            if r < cdf:
                return int(t)
        return int(idx[last])


def main():
    model_path, tok_path, prompt, steps, temperature, topp, seed = sys.argv[1:8]
    m = load_model(model_path)
    tok = load_tokenizer(tok_path, m["vocab"])
    sampler = Sampler(float(temperature), float(topp), int(seed))
    steps = int(steps)
    if steps <= 0 or steps > m["seq"]:
        steps = m["seq"]
    ids = encode(tok, prompt)
    out = bytearray()
    token, pos = ids[0], 0
    while pos < steps:
        logits = forward(m, token, pos)
        nxt = ids[pos + 1] if pos < len(ids) - 1 else sampler.sample(logits)
        pos += 1
        if nxt == 1:
            break
        piece = decode(tok, token, nxt)
        if len(piece) != 1 or 32 <= piece[0] < 127 or piece[0] in (9, 10, 11, 12, 13):
            out += piece
        token = nxt
    sys.stdout.buffer.write(bytes(out) + b"\n")


if __name__ == "__main__":
    main()
