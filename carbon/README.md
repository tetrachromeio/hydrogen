# Carbon

Local LLM inference in Hydrogen (element 6). The plan and milestones are in
[docs/CARBON.md](../docs/CARBON.md).

**Status: M1 and M2 done: correct, SIMD, multithreaded, Q8_0 quantized.** Next: M3, ternary weights. Carbon runs Llama-architecture models
in llama2.c's formats, f32 or Q8_0, for example [stories15M / 42M / 110M](https://huggingface.co/karpathy/tinyllamas):
- Weights are memory-mapped from the file and used in place, never copied or parsed into objects.
- All activations and the key/value cache come from one `Arena` allocation, made once.
- Grouped-query attention, RoPE, SwiGLU feed-forward.
- The SentencePiece-style tokenizer, with byte fallback for any UTF-8 text.
- Greedy, temperature and top-p sampling, with the same random numbers as llama2.c.
- Q8_0 quantization (int8 in groups with an f32 scale, llama2.c's "version 2" files): a quarter
  of the size, integer dot products. `examples/quantize` converts an f32 model.

Its output matches an independent numpy implementation (`tests/carbon/reference.py`)
token for token, greedy and sampled.

```bash
cd carbon/examples/quantize && hy build --release && build/quantize stories42M.bin stories42M_q8.bin
cd ../generate && hy build --release --native
build/generate stories42M_q8.bin -z tokenizer.bin -i "Once upon a time" -n 200 -t 0.8 -s 42
```

Options are llama2.c's: `-z` tokenizer, `-i` prompt, `-n` steps, `-t` temperature
(0 means greedy), `-p` top-p, `-s` seed; plus `-T` threads (default: all cores).

## As a library

```
use Carbon

match load_model("stories15M.bin") {
    Ok(model) => {
        let tok = load_tokenizer("tokenizer.bin", model.config.vocab_size)   // Result
        var sampler = Sampler(model.config.vocab_size, 0.8, 0.9, 42)
        generate(model, tok, sampler, "Once upon a time", 256, cpu_count(), function(piece: str) {
            // each new piece of text, as soon as it exists (send it to a Channel, a socket, a Boron UI...)
        })
    }
    Err(e) => { eprint(e) }
}
```

Lower level: `State(model.config, threads)`, then `model.forward(state, token, pos)` returns the logits;
`tok.encode(text, bos, eos)` / `tok.decode(prev, token)`; `sampler.sample(logits)`.

## Speed

x86-64 container with 4 cores, `--release --native`, tokens per second:

| model | f32, 1 thread | f32, 4 threads | Q8_0, 1 thread | Q8_0, 4 threads |
|---|---|---|---|---|
| stories15M (61 MB → 17 MB) | ~380 | ~840 | ~500 | ~880 |
| stories42M (167 MB → 47 MB) | ~54 | ~290 | ~146 | ~530 |

How we got here (stories15M, one thread): the scalar M1 loop managed ~50, because
each multiply-add waited for the previous one. SIMD `f32x8` kernels with four
accumulators reached ~290, and `--native` (AVX2) ~380. A persistent thread
`Pool` splits each large matrix multiply by rows. Q8_0 moves a quarter of the
bytes: one core can't read f32 stories42M fast enough, but it can read Q8_0.

**Correctness:**
- **f32, and tiny-model Q8_0:** the text matches the numpy reference (`tests/carbon/reference.py`)
  token for token.
- **Quantizer:** the file Carbon writes is byte-identical to the reference's.
- **Real Q8_0 models:** these can part ways at near-ties. On stories15M the
  reference's top two logits at the first difference were 16.989 vs 16.942:
  float rounding noise moves activations across int8 rounding boundaries.

## Files

| | |
|---|---|
| `kernels.h2` | `Matrix` views, SIMD `dot`/`axpy`/`matvec`, `matvec_pool`, `rmsnorm`, `softmax`, `argmax` |
| `quant.h2` | `Weights` (f32 or Q8_0), `quantize`, Q8_0 matrix-vector product, `matmul` |
| `quantize.h2` | `quantize_model`: f32 file → Q8_0 file |
| `model.h2` | `Config`, `Model` (both file formats, mapped weights, `forward`), `State` (arena, pool) |
| `tokenizer.h2` | `Tokenizer`: `encode`, `decode` |
| `sampler.h2` | `Sampler`: greedy, temperature, top-p |
| `generate.h2` | `generate`: prompt prefill + decode loop, streaming each piece |
