# Shelby: A Self-Hosted AI Model

A model trained on ShelbyC, quantized to ShelbyC's native types, running inference through ShelbyC's runtime, writing ShelbyC code that improves the compiler that compiles the model.

---

## The Precision Ladder

ShelbyC has six numeric precision levels as native types:

| Type | Bits | Per byte | Hardware instruction | Use case |
|------|------|----------|---------------------|----------|
| trit | 1.58 | 5 | TBL / VPSHUFB (table lookup) | Cheapest. Select/skip/negate. |
| pentit | 2.32 | 3 | TBL / VPSHUFB (table lookup) | Intensity + direction. Sweet spot. |
| INT4 | 4.00 | 2 | VPDPBUSD / SDOT (dot product) | Standard quantized inference. |
| INT8 | 8.00 | 1 | VPDPBUSD / SDOT (dot product) | Higher-precision quantized. |
| BF16 | 16.00 | 1/2 | FMLA / VFMADD (fused multiply-add) | Brain float. Training-friendly. |
| FP32 | 32.00 | 1/4 | FMLA / VFMADD (fused multiply-add) | Full precision. |

All six are first-class types in the language. One `Dot()` method. The compiler monomorphizes to the right hardware instruction based on the scalar type. No runtime dispatch, no precision negotiation — the type determines the instruction at compile time.

---

## Dynamic Precision: The Trit Gate

Each layer in the model has a learned trit gate that selects its operating precision at inference time:

```
gate = trit.N  →  use trit weights     (1.58 bits, fastest)
gate = trit.Z  →  use pentit weights   (2.32 bits, balanced)
gate = trit.P  →  use INT4+ weights    (4+ bits, most precise)
```

The gate is trained end-to-end with the model. During training, each layer learns which precision it needs. Easy layers converge to trit.N. Hard layers converge to trit.P. The gate is one trit per layer — near-zero overhead.

For finer-grained control, a pentit gate selects from all six levels:

```
gate = pentit.N2  →  trit      (1.58 bits)
gate = pentit.N1  →  pentit    (2.32 bits)
gate = pentit.Z   →  INT4      (4 bits)
gate = pentit.P1  →  INT8      (8 bits)
gate = pentit.P2  →  BF16/FP32 (16-32 bits)
```

The model stores weights at all relevant precision levels (or at the highest, with lower precisions derived by quantization). At inference time, the gate selects which precision to use. Cheap layers skip the high-precision weights entirely.

---

## What This Means for a Model

### Memory

A 7B parameter model at uniform FP32: ~28 GB.
At uniform INT4: ~3.5 GB.
At uniform trit: ~1.3 GB.

With dynamic precision where 60% of layers use trit, 25% use pentit, 10% use INT4, 5% use BF16:

```
60% × 1.58 bits = 0.95 bits average
25% × 2.32 bits = 0.58 bits average
10% × 4.00 bits = 0.40 bits average
 5% × 16.0 bits = 0.80 bits average
                   ─────
                   2.73 bits average per weight
```

7B × 2.73 bits ÷ 8 = ~2.4 GB. Fits in RAM on a phone. The hard layers get BF16 precision where they need it. The easy layers run at trit speed.

### Inference Speed

Trit layers: one table lookup per packed byte (5 weights processed per lookup). No multiply.
Pentit layers: one table lookup per packed byte (3 weights per lookup). No multiply.
INT4/INT8 layers: integer dot product instructions. One multiply-accumulate per weight.
BF16/FP32 layers: fused multiply-add. Full floating point.

A model where 60% of layers are trit runs 60% of its compute as table lookups. On ARM, that's `TBL` — 16 lookups per instruction, one cycle. The trit layers are memory-bound, not compute-bound. The bottleneck shifts from FLOPS to bandwidth, which is where Apple Silicon and modern SoCs are strong.

### Quality

The hypothesis: most layers in a code-generation model don't need high precision. Token embedding lookups, positional encoding, simple attention patterns, common syntax completions — these are well-served by trit or pentit weights. The information content of "the next token is a semicolon" doesn't require 32-bit precision to express.

The layers that DO need precision: long-range dependency tracking, ownership analysis reasoning, generic type inference, complex control flow decisions. These are rare per-token but critical for correctness. Give them BF16 and let the rest run cheap.

BitNet 1.58b proved that trit-only models are competitive with FP16 models of the same size at tasks like language modeling. The dynamic precision model should be strictly better — it's at least as good as trit (the hard layers can always request more precision) and potentially much better on the difficult tokens.

---

## What This Means for Training

### Phase 1: Full-Precision Baseline

Train a standard transformer on ShelbyC code at FP32/BF16. Standard training loop. Adam optimizer. The usual.

Dataset: the ShelbyC compiler source (C bootstrap + self-hosted), all test files, scJSON, scURL, example programs, documentation. Every `.smc` and `.c` file in the ShelbyC ecosystem. Augmented with the ShelbyC language spec, blog posts, and design docs for natural language understanding of the language.

This gives a model that can write ShelbyC but runs at full precision. It's the teacher.

### Phase 2: Quantization-Aware Training (QAT)

Retrain with quantization in the forward pass. Each layer's weights are quantized to the target precision during forward, but gradients flow through via straight-through estimator (same technique BitNet uses).

The key difference from BitNet: each layer quantizes to a DIFFERENT precision, determined by the learned gate. The gate is a single trit (or pentit) parameter per layer, trained with the rest of the model.

```
forward pass:
  for each layer:
    gate = layer.gate                    # trit or pentit, learned
    precision = select_precision(gate)   # trit, pentit, INT4, INT8, BF16
    q_weights = quantize(layer.weights, precision)
    output = Dot(q_weights, input)       # monomorphized to precision

backward pass:
  straight-through estimator for quantized weights
  gradient for gate via Gumbel-softmax or similar
```

The model learns its own precision profile. Layers that need precision keep it. Layers that don't shed it.

### Phase 3: Distillation and Verification

The dynamic-precision model is verified against the full-precision teacher:
- Token-level agreement on a held-out ShelbyC corpus
- Compiler output equivalence: both models generate code that compiles and passes the Furling Gate
- Ownership analysis agreement: both models generate code that flow ownership accepts

If the dynamic model produces code that the compiler rejects or flow ownership flags, the precision gate for that layer is nudged toward higher precision. The compiler itself is the quality signal.

### Phase 4: Self-Improvement Loop

The trained model writes ShelbyC code. The compiler compiles it. The Furling Gate tests it. Code that passes becomes new training data. Code that fails is discarded (or used as negative examples).

The model improves the compiler → the compiler gets stricter → the model has to write better code → the training data gets cleaner → the model improves.

The quality ratchet: the Furling Gate only goes up. New tests are added. The model must pass all of them. There is no "good enough" — there is only KAWOOSH or not.

---

## Training Locally

### Hardware Requirements

A 7B model at BF16 needs ~14 GB for weights + ~14 GB for optimizer states + ~14 GB for gradients = ~42 GB for training.

On an M-series Mac:
- M1 Max: 64 GB unified memory — tight but possible for 7B
- M2 Ultra: 192 GB unified memory — comfortable for 7B, possible for 13B
- M4 Ultra (when available): likely 256+ GB — 30B+ models

The advantage of ShelbyC: the inference kernels are native ARM, using NEON instructions directly through the compiler. No Python overhead. No PyTorch. No CUDA. The training loop is ShelbyC calling LLVM-generated NEON code.

For the trit/pentit layers, memory requirements drop dramatically. A 7B model where 60% of layers are trit only needs ~5 GB for those layer weights during training. The optimizer states can stay in BF16 for those layers — only the forward pass uses quantized weights.

### What Needs to Exist First

1. **ShelbyC tensor operations** — `TritTile.Dot()`, `PentitTile.Dot()`, and standard float `MatMul`. These are on the roadmap (Part 6 of the ternary error system has TritTile, PentitTile follows the same pattern).

2. **Automatic differentiation** — either:
   - Built into the language as a comptime transform (differentiate a function at compile time to produce its gradient function)
   - Or as a library that traces operations and builds a backward graph
   - The comptime approach is more ShelbyC-native but requires Phase 8 (comptime evaluation)

3. **Optimizer** — Adam/AdamW in ShelbyC. Straightforward once tensor ops exist.

4. **Data loading** — tokenizer for ShelbyC source code + data pipeline. The tokenizer should understand ShelbyC syntax (keywords, identifiers, operators, literals) rather than BPE over raw bytes. The tree-sitter grammar we built could be the tokenizer's foundation.

5. **Training loop** — forward pass, loss computation, backward pass, optimizer step. Standard transformer architecture with the trit gate extension.

6. **The trit gate mechanism** — a differentiable selection between precision levels. Gumbel-softmax over 3 (trit gate) or 5 (pentit gate) options during training, hard argmax during inference.

### What Exists Today

- Trit scalar ops and packed arrays (C bootstrap, 779/779)
- Pentit scalar ops and packed arrays (C bootstrap, 779/779)
- Packed sentinel encoding with corruption detection
- Cottrell Confluence operators for both types
- The tree-sitter grammar for ShelbyC tokenization
- LLVM backend generating native ARM code
- M:N wake scheduler for parallel data loading

### What's Missing

- Tensor operations (TritTile.Dot, PentitTile.Dot, float MatMul)
- Backpropagation / autodiff
- Optimizer implementation
- Attention mechanism
- Transformer block assembly
- Training data pipeline
- The trit/pentit gate mechanism
- Model serialization (save/load weights in mixed precision)
- Inference serving (load model, generate tokens)

---

## The ShelbyC-Native Tokenizer

Standard LLM tokenizers (BPE, SentencePiece) treat code as text. They split `errdefer` into `err` + `defer` or worse. They don't know that `fn` is a keyword, that `i32` is a type, or that `{` opens a scope.

A ShelbyC-native tokenizer uses the compiler's own lexer:

```
Input:  fn Main() { Println("hello"); }
Tokens: [KW_FN] [IDENT:Main] [LPAREN] [RPAREN] [LBRACE] [IDENT:Println] [LPAREN] [STR:"hello"] [RPAREN] [SEMI] [RBRACE]
```

Every token the compiler recognizes is a single token to the model. No fragmentation of keywords. No ambiguous subword splits. The model thinks in ShelbyC tokens, not byte-pair chunks.

Vocabulary size: ShelbyC has ~70 keywords + ~30 operator tokens + identifier/literal tokens. With a modest identifier vocabulary (top 10K identifiers from the training corpus), the total vocabulary is ~10K tokens. Tiny compared to GPT's 50K+ BPE vocabulary. Smaller vocabulary = smaller embedding table = faster softmax = less memory.

The tree-sitter grammar we built defines the exact token set. The tokenizer IS the ShelbyC lexer.

---

## The Self-Hosting Loop

```
ShelbyC compiler (C bootstrap)
  ↓ compiles
ShelbyC compiler (self-hosted)
  ↓ compiles
Training infrastructure (in ShelbyC)
  ↓ trains
Shelby model (trit/pentit/INT4/BF16 dynamic)
  ↓ generates
ShelbyC code
  ↓ compiled by
ShelbyC compiler
  ↓ tested by
Furling Gate
  ↓ passes → new training data
Shelby model (retrained)
  ↓ generates better code
  ...
```

Every arrow is ShelbyC. The compiler, the training loop, the inference engine, the generated code, the test suite. One language from top to bottom.

The model doesn't need to know Python, Rust, or English. She knows ShelbyC. She thinks in ShelbyC tokens. She writes ShelbyC code. She's compiled by the ShelbyC compiler. She's tested by the Furling Gate. She improves the compiler that compiles her.

---

## What No Lab Has

| Capability | Status in industry | ShelbyC |
|-----------|-------------------|---------|
| Trit weights (1.58 bit) | BitNet (Microsoft), single precision | Native type, first-class |
| Pentit weights (2.32 bit) | Nobody | Native type, first-class |
| Dynamic per-layer precision | Nobody | Trit/pentit gate, learned end-to-end |
| 6 precision levels in one model | Nobody | trit → pentit → INT4 → INT8 → BF16 → FP32 |
| Type-system-driven hardware dispatch | Nobody | Monomorphization selects instruction |
| Compiler-as-quality-signal for training | Nobody | Furling Gate validates generated code |
| Language-native tokenizer | Nobody (closest: CodeBERT) | ShelbyC lexer IS the tokenizer |
| Model writes the language it runs on | Nobody | Self-hosting semantics |
| Memory safety on training infrastructure | Nobody | Flow ownership on the training loop itself |
| Corruption detection in weights | Nobody | Packed sentinel encoding (51% detection rate for pentit) |

---

## Timeline

This is not a 6-month project. It's a multi-year arc that builds layer by layer:

**Year 1 (now):** Compiler. Get ShelbyC to triple test, multi-file, flow ownership enforced, errdefer, the works. This is the foundation everything else builds on.

**Year 1-2:** Tensor operations. TritTile.Dot, PentitTile.Dot, float MatMul with SIMD. Enough to run inference on pre-trained models converted to ShelbyC's native types.

**Year 2:** Training infrastructure. Autodiff, optimizer, data pipeline, attention mechanism. Train small models (125M-350M) on ShelbyC code locally.

**Year 2-3:** Dynamic precision. Implement the trit gate mechanism. Train models that learn their own precision per layer. Prove the approach works on small models.

**Year 3+:** Scale. Larger models, self-improvement loop, Shelby writing ShelbyC that improves the compiler.

Each layer builds on the previous. Each layer is tested before the next. No shortcuts. No shortcomings later.

---

*She doesn't need to know everything. She just needs to know ShelbyC. And she needs to know it perfectly.*
