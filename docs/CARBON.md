# Carbon: plan

Carbon (element 6) is a local AI inference engine written in Hydrogen. It loads
weights straight from disk with mmap, runs with no garbage collector, and keeps
memory flat during generation. This page reviews the original blueprint, lists
what Hydrogen needs first, and sets the milestones.

## The design, with corrections

**Weights: mmap and zero-copy views.** This part is right as written. The model
file is mapped read-only. Tensors are small structs holding shape, strides and a
typed pointer into the mapping. Notes:
- Zero-copy works only if the file is already in compute layout and aligned.
  GGUF qualifies, so Carbon reads GGUF rather than inventing a format.
- Pages load lazily, so the first token is slow. Use `madvise(MADV_WILLNEED)`, or
  `mlock` where allowed.

**Activations: an arena.** Allocate it once per loaded model, not per request:
decode activation sizes are fixed by the model's dimensions. Hydrogen doesn't
borrow-check views yet, so a view into the arena can dangle after a reset. The
code must not keep views past a reset.

**Ternary weights (BitNet b1.58).**
- Ternary needs a model *trained* ternary. Post-quantizing an ordinary model to
  {-1, 0, 1} destroys it. Target BitNet b1.58 2B4T.
- Packing: 5 trits per byte (3⁵ = 243) is densest but needs a lookup table to
  decode. 2 bits per weight (bitnet.cpp's I2_S) vectorizes cleanly. Start with
  2-bit; add the TL1/TL2 lookup-table kernels later.
- "FPU bypass" covers only the linear layers. Activations are quantized to int8
  (absmax), so the ternary matmul becomes int8 adds and subtracts. RMSNorm,
  RoPE, softmax and the attention products stay in float.
- Decode speed is bound by memory bandwidth. The win from 1.58-bit weights is
  moving about 10× fewer bytes per token; skipping multiplies matters less.

**KV cache and memory.**
- The KV cache grows O(N) per sequence. Attention *compute* over a whole
  generation is O(N²), O(N) per token.
- A rolling-window cache is valid only for models trained with sliding-window
  attention (Mistral v0.1, Gemma's local layers). Otherwise keep a few
  "attention sink" tokens (StreamingLLM). Overwriting old tokens in an ordinary
  model breaks its output once the window wraps.
- **SSMs (Mamba-2)** keep a fixed-size state: O(1) memory per token, for real.
  They are a separate model family; no good ternary SSM checkpoint exists today.

**Pipeline.**
- The tokenizer is BPE: a hash map from merge pair to rank (`Elemental.Map`) and
  an iterative merge loop. A trie with greedy longest-match gives the wrong tokens.
- Prefill is matrix × matrix across all prompt tokens, split over cores.
- Decode is matrix × vector, one token at a time.
- Sampling (temperature, top-k, top-p) runs in place on the logits buffer.

**Concurrency.**
- `Sendable` prevents data races at compile time. `Channel` still uses a mutex,
  which costs nothing per token next to a forward pass.
- Streaming: the worker sends tokens over a `Channel`. Oxygen turns them into
  SSE; a Boron desktop app reads the channel directly in the same process, with
  no HTTP.

## What Hydrogen needs first

1. **SIMD vector types:** `vec<f32, 8>`, `vec<i8, 32>` and so on, lowered to LLVM
   `<N x T>`, with intrinsics (dot, maddubs, shuffle, horizontal add). Inline `asm`
   can't keep values in vector registers between blocks.
2. **CPU targeting:** `hy build --release --native`, or `target-cpu` in `hy.toml`,
   to enable AVX2/AVX-512/NEON.
3. **`f16` / `bf16`** types, with conversions.
4. **Read-only sharing across threads, plus a thread pool and a barrier.**
   Prefill needs persistent workers that all read the same mapped weights and
   synchronize at each layer. That means a `Shared<T>` (or a read-only pointer
   marked `Sendable`) and a spin barrier on the existing atomics.
5. **Elemental additions:** `map_file` (mmap/madvise), an `Arena` type, and
   `noalias` pointer parameters for the auto-vectorizer.
6. **A benchmark harness:** `hy bench`, or `Elemental.Time` plus a small library.

## Milestones

| | Goal | Proves |
|---|---|---|
| M0 + M1 ✅ | mmap, tensor views, arena; llama2.c `stories15M` (fp32), scalar, matching an independent reference token for token | correctness baseline |
| M2 (in progress) | ✅ SIMD vector types, ✅ thread pool (~16× over M1); Q8_0, GGUF reader | speed |
| M3 | BitNet b1.58 2B4T with 2-bit ternary kernels | the ternary claim |
| M4 | Sliding window and attention sinks | flat memory on transformers |
| M5 | Mamba-2 | O(1)-memory mode |

At every milestone, correctness means matching a reference implementation's
logits within tolerance on fixed prompts.
