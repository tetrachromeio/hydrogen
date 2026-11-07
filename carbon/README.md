# Carbon

Local LLM inference in Hydrogen (element 6). The plan and milestones are in
[docs/CARBON.md](../docs/CARBON.md).

**Status: M1 done; M2 mostly done (SIMD, threads); quantization next.** Carbon runs Llama-architecture models
in llama2.c's format, for example [stories15M](https://huggingface.co/karpathy/tinyllamas):
- Weights are memory-mapped from the file and used in place, never copied or parsed into objects.
- All activations and the key/value cache come from one `Arena` allocation, made once.
- Grouped-query attention, RoPE, SwiGLU feed-forward.
- The SentencePiece-style tokenizer, with byte fallback for any UTF-8 text.
- Greedy, temperature and top-p sampling, with the same random numbers as llama2.c.

Its output matches an independent numpy implementation (`tests/carbon/reference.py`)
token for token, greedy and sampled.

```bash
cd carbon/examples/generate
hy build --release --native
build/generate stories15M.bin -z tokenizer.bin -i "Once upon a time" -n 200 -t 0.8 -s 42
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

stories15M, one thread, x86-64 container, 256 tokens:

| build | tok/s |
|---|---|
| M1: scalar, one accumulator (llama2.c's order of operations) | ~50 |
| SIMD `f32x8`, four accumulators, `--release` | ~290 |
| same, `--release --native` (AVX2) | ~380 |
| same, 2 / 3 / 4 threads (`-T`) | ~550 / ~760 / ~790 |

- **SIMD:** each multiply-add in M1 waited for the previous one; the SIMD
  kernels keep 32 products in flight.
- **Threads:** a persistent `Pool` (Elemental.Thread) splits each large matrix
  multiply by rows; handing out a round costs a few microseconds.
- **Output stays correct:** at every thread count it still matches the
  reference token for token.
- **Next:** quantized weights (Q8_0, then ternary). Decode becomes limited by
  memory bandwidth there, so fewer bytes per weight is the next big win.

## Files

| | |
|---|---|
| `kernels.h2` | `Matrix` views, `matvec`, `rmsnorm`, `softmax`, `argmax` |
| `model.h2` | `Config`, `Model` (mapped weights, `forward`), `State` (arena) |
| `tokenizer.h2` | `Tokenizer`: `encode`, `decode` |
| `sampler.h2` | `Sampler`: greedy, temperature, top-p |
| `generate.h2` | `generate`: prompt prefill + decode loop, streaming each piece |
