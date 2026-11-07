#!/usr/bin/env python3
"""An independent numpy implementation of the llama2.c model, tokenizer and
sampler, to check Carbon against.

  reference.py <model.bin|model.gguf> <tokenizer.bin|-> <prompt> <steps> <temperature> <topp> <seed> [chat]
  reference.py --quantize <f32 model.bin> <out.bin> <group size>

The first prints the generated text, like Carbon's generate example; the model
may be f32 (llama2.c version 0) or Q8_0 (version 2). The second writes a Q8_0
file, to compare byte for byte with Carbon's quantizer. Arithmetic is float32
throughout; numpy sums f32 dot products in a different order than Carbon, so
logits agree to about 1e-5, not bit for bit (Q8_0 integer sums are exact).
"""
import struct
import sys

import numpy as np


def round_away(v):
    v = v.astype(np.float64)
    return np.where(v < 0, -np.floor(-v + 0.5), np.floor(v + 0.5))


def quantize(x, gs):
    """x: float32 array (length a multiple of gs) -> (int8 values, float32 scales)."""
    g = x.reshape(-1, gs)
    scale = (np.abs(g).max(axis=1) / np.float32(127.0)).astype(np.float32)
    safe = np.where(scale == 0, np.float32(1), scale)
    q = np.where(scale[:, None] == 0, 0, round_away((g / safe[:, None]).astype(np.float32))).astype(np.int8)
    return q.reshape(-1), scale


class Q8:
    """A Q8_0 matrix: q (rows, cols) int8, s (rows, cols / gs) float32."""

    def __init__(self, q, s, gs):
        self.q, self.s, self.gs = q, s, gs

    def row(self, r):
        return (self.q[r].astype(np.float32) * np.repeat(self.s[r], self.gs)).astype(np.float32)

    def __matmul__(self, x):
        xq, xs = quantize(x, self.gs)
        rows, cols = self.q.shape
        ival = (self.q.astype(np.int32).reshape(rows, cols // self.gs, self.gs) *
                xq.astype(np.int32).reshape(1, cols // self.gs, self.gs)).sum(axis=2)
        val = np.zeros(rows, np.float32)
        for g in range(cols // self.gs):
            val = val + (ival[:, g].astype(np.float32) * self.s[:, g]) * xs[g]
        return val


def load_q8(raw):
    dim, hidden, n_layers, n_heads, n_kv, vocab, seq_len = struct.unpack_from("7i", raw, 8)
    shared = raw[36] != 0
    gs = struct.unpack_from("i", raw, 37)[0]
    head = dim // n_heads
    kv_dim = dim * n_kv // n_heads
    at = 256

    def f32(*shape):
        nonlocal at
        n = int(np.prod(shape))
        out = np.frombuffer(raw, np.float32, n, at).reshape(shape)
        at += n * 4
        return out

    def q8(layers, rows, cols):
        nonlocal at
        out = []
        for _ in range(layers):
            n = rows * cols
            q = np.frombuffer(raw, np.int8, n, at).reshape(rows, cols)
            s = np.frombuffer(raw, np.float32, n // gs, at + n).reshape(rows, cols // gs)
            at += n + n // gs * 4
            out.append(Q8(q, s, gs))
        return out

    m = dict(dim=dim, hidden=hidden, L=n_layers, H=n_heads, KV=n_kv, vocab=vocab, seq=seq_len,
             head=head, kv_dim=kv_dim)
    m["rms_att"] = f32(n_layers, dim)
    m["rms_ffn"] = f32(n_layers, dim)
    m["rms_final"] = f32(dim)
    embed = q8(1, vocab, dim)[0]
    m["embed_q"] = embed
    for name, rows, cols in [("wq", dim, dim), ("wk", kv_dim, dim), ("wv", kv_dim, dim), ("wo", dim, dim),
                             ("w1", hidden, dim), ("w2", dim, hidden), ("w3", hidden, dim)]:
        m[name] = q8(n_layers, rows, cols)
    m["wcls"] = embed if shared else q8(1, vocab, dim)[0]
    m["kcache"] = np.zeros((n_layers, seq_len, kv_dim), np.float32)
    m["vcache"] = np.zeros((n_layers, seq_len, kv_dim), np.float32)
    return m


def write_q8(src, out, gs):
    raw = open(src, "rb").read()
    m = load_model(src)
    shared = struct.unpack_from("i", raw, 20)[0] > 0
    header = struct.pack("<Ii7iBi", 0x616B3432, 2, m["dim"], m["hidden"], m["L"], m["H"], m["KV"], m["vocab"],
                         m["seq"], int(shared), gs)
    parts = [header + bytes(256 - len(header))]
    parts += [m["rms_att"].tobytes(), m["rms_ffn"].tobytes(), m["rms_final"].tobytes()]

    def put(t):
        q, s = quantize(np.ascontiguousarray(t).reshape(-1), gs)
        parts.extend([q.tobytes(), s.tobytes()])

    put(m["embed"])
    for name in ["wq", "wk", "wv", "wo", "w1", "w2", "w3"]:
        for l in range(m["L"]):
            put(m[name][l])
    if not shared:
        put(m["wcls"])
    open(out, "wb").write(b"".join(parts))


def load_gguf(path):
    raw = np.memmap(path, dtype=np.uint8, mode="r")
    buf = raw.tobytes() if len(raw) < (64 << 20) else bytes(raw[: 64 << 20])
    at = 8
    n_tensors, n_kv = struct.unpack_from("<QQ", buf, at)
    at += 16
    sizes = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 1, 10: 8, 11: 8, 12: 8}
    fmts = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "?", 10: "Q", 11: "q", 12: "d"}

    def text():
        nonlocal at
        n, = struct.unpack_from("<Q", buf, at)
        at += 8
        out = buf[at:at + n]
        at += n
        return out

    def value(t):
        nonlocal at
        if t == 8:
            return text()
        if t == 9:
            et, n = struct.unpack_from("<IQ", buf, at)
            at += 12
            if et == 8:
                return [text() for _ in range(n)]
            out = np.frombuffer(buf, "<" + fmts[et], n, at)
            at += sizes[et] * n
            return out
        v, = struct.unpack_from("<" + fmts[t], buf, at)
        at += sizes[t]
        return v

    meta = {}
    for _ in range(n_kv):
        k = text().decode()
        t, = struct.unpack_from("<I", buf, at)
        at += 4
        meta[k] = value(t)
    tensors = {}
    for _ in range(n_tensors):
        name = text().decode()
        nd, = struct.unpack_from("<I", buf, at)
        at += 4
        dims = struct.unpack_from("<" + "Q" * nd, buf, at)
        at += 8 * nd
        t, off = struct.unpack_from("<IQ", buf, at)
        at += 12
        tensors[name] = (t, dims, off)
    align = meta.get("general.alignment", 32)
    base = (at + align - 1) // align * align
    block = np.dtype([("d", "<f2"), ("qs", "i1", 32)])

    def get(name):
        t, dims, off = tensors[name]
        cols, rows = dims[0], (dims[1] if len(dims) > 1 else 1)
        if t == 0:
            a = np.frombuffer(raw, np.float32, rows * cols, base + off)
            return a if len(dims) == 1 else a.reshape(rows, cols)
        assert t == 8, f"{name}: ggml type {t}"
        b = np.frombuffer(raw, block, rows * cols // 32, base + off).reshape(rows, cols // 32)
        return Q8(b["qs"].reshape(rows, cols), b["d"].astype(np.float32), 32)

    a = meta["general.architecture"].decode()
    dim, n_layers, n_heads = meta[a + ".embedding_length"], meta[a + ".block_count"], meta[a + ".attention.head_count"]
    n_kv = meta.get(a + ".attention.head_count_kv", n_heads)
    vocab = len(meta["tokenizer.ggml.tokens"])
    m = dict(dim=dim, hidden=meta[a + ".feed_forward_length"], L=n_layers, H=n_heads, KV=n_kv, vocab=vocab,
             seq=meta[a + ".context_length"], head=dim // n_heads, kv_dim=dim * n_kv // n_heads,
             eps=np.float32(meta.get(a + ".attention.layer_norm_rms_epsilon", 1e-5)),
             theta=np.float32(meta.get(a + ".rope.freq_base", 10000.0)),
             bos=meta.get("tokenizer.ggml.bos_token_id", 1), eos=meta.get("tokenizer.ggml.eos_token_id", 2))
    m["embed_q"] = get("token_embd.weight")
    m["wcls"] = get("output.weight") if "output.weight" in tensors else m["embed_q"]
    m["rms_final"] = get("output_norm.weight")
    for key, name in [("rms_att", "attn_norm"), ("rms_ffn", "ffn_norm"), ("wq", "attn_q"), ("wk", "attn_k"),
                      ("wv", "attn_v"), ("wo", "attn_output"), ("w1", "ffn_gate"), ("w2", "ffn_down"),
                      ("w3", "ffn_up")]:
        m[key] = [get(f"blk.{l}.{name}.weight") for l in range(n_layers)]
    m["kcache"] = np.zeros((n_layers, m["seq"], m["kv_dim"]), np.float32)
    m["vcache"] = np.zeros((n_layers, m["seq"], m["kv_dim"]), np.float32)
    pieces = [p.replace("\u2581".encode(), b" ") for p in meta["tokenizer.ggml.tokens"]]
    scores = [float(x) for x in meta["tokenizer.ggml.scores"]]
    lookup = {}
    for i, p in enumerate(pieces):
        lookup.setdefault(p, i)
    m["tokenizer"] = (pieces, scores, lookup)
    return m


def load_model(path):
    if open(path, "rb").read(4) == b"GGUF":
        return load_gguf(path)
    raw = open(path, "rb").read()
    if struct.unpack_from("I", raw, 0)[0] == 0x616B3432:
        return load_q8(raw)
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


def rmsnorm(x, weight, eps=np.float32(1e-5)):
    ss = np.float32(np.mean(x * x, dtype=np.float32)) + eps
    return weight * (x / np.sqrt(ss)).astype(np.float32)


def softmax(x):
    e = np.exp(x - x.max())
    return (e / e.sum()).astype(np.float32)


def forward(m, token, pos):
    head, H, kv_dim = m["head"], m["H"], m["kv_dim"]
    kv_mul = H // m["KV"]
    x = m["embed_q"].row(token) if "embed_q" in m else m["embed"][token].copy()
    # RoPE angles for this position
    i = np.arange(0, m["dim"], 2)
    freq = (1.0 / np.power(np.float32(m.get("theta", 10000.0)), (i % head).astype(np.float32) / np.float32(head))).astype(np.float32)
    ang = np.float32(pos) * freq
    cos, sin = np.cos(ang).astype(np.float32), np.sin(ang).astype(np.float32)

    def rope(v):
        v0, v1 = v[0::2].copy(), v[1::2].copy()
        n = len(v0)
        v[0::2] = v0 * cos[:n] - v1 * sin[:n]
        v[1::2] = v0 * sin[:n] + v1 * cos[:n]

    for l in range(m["L"]):
        xb = rmsnorm(x, m["rms_att"][l], m.get("eps", np.float32(1e-5)))
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
        xb = rmsnorm(x, m["rms_ffn"][l], m.get("eps", np.float32(1e-5)))
        h1 = m["w1"][l] @ xb
        h3 = m["w3"][l] @ xb
        hb = (h1 * (np.float32(1.0) / (np.float32(1.0) + np.exp(-h1)))) * h3
        x = x + m["w2"][l] @ hb.astype(np.float32)
    x = rmsnorm(x, m["rms_final"], m.get("eps", np.float32(1e-5)))
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


def encode(tok, text, bos=True, prefix=True):
    pieces, scores, lookup = tok
    data = text.encode()
    ids = [1] if bos else []
    if data and prefix:
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
    if sys.argv[1] == "--quantize":
        write_q8(sys.argv[2], sys.argv[3], int(sys.argv[4]))
        return
    model_path, tok_path, prompt, steps, temperature, topp, seed = sys.argv[1:8]
    chat = len(sys.argv) > 8 and sys.argv[8] == "chat"
    m = load_model(model_path)
    tok = m["tokenizer"] if "tokenizer" in m else load_tokenizer(tok_path, m["vocab"])
    bos, eos = m.get("bos", 1), m.get("eos", 2)
    sampler = Sampler(float(temperature), float(topp), int(seed))
    steps = int(steps)
    if steps <= 0 or steps > m["seq"]:
        steps = m["seq"]
    if chat:
        ids = encode(tok, "<|user|>\n" + prompt) + [eos] + encode(tok, "\n<|assistant|>\n", False, False)
    else:
        ids = encode(tok, prompt)
    out = bytearray()
    token, pos = ids[0], 0
    while pos < steps:
        logits = forward(m, token, pos)
        in_prompt = pos < len(ids) - 1
        nxt = ids[pos + 1] if in_prompt else sampler.sample(logits)
        if not in_prompt and nxt in (bos, eos):
            break
        pos += 1
        piece = decode(tok, token, nxt)
        if len(piece) != 1 or 32 <= piece[0] < 127 or piece[0] in (9, 10, 11, 12, 13):
            if not (chat and in_prompt):
                out += piece
        token = nxt
    sys.stdout.buffer.write(bytes(out) + b"\n")


if __name__ == "__main__":
    main()
