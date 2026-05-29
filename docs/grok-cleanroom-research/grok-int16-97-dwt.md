# Grok 16-bit 9/7 Inverse DWT — Operational Detail

Supplement to `grok-performance-innovations.md` §5.5, written to
support SP-3.5 implementation of an analogous path in openjp2k.

**Cleanroom status:** describer document. Author read Grok source
(`src/lib/core/wavelet/WaveletReverse97_16.cpp`, ~250-line pedagogical
header comment plus the scalar synthesis kernel). No Grok code is
reproduced here. Constant values listed below are mechanical Q1.15
encodings of standard T.800 Annex F coefficients — they fall out of
the standard, not from Grok-specific tuning, and are reusable under the
cleanroom rules.

**Implementers:** read this document and `grok-performance-innovations.md`
§5.5. Do **not** read Grok source.

## 1. Q-format and primitive

All intermediate state is `int16_t` in Q1.15. The single multiply
primitive is rounding multiply-high:

```
opj_q15_mul(a, b) = (int16_t)(((int32_t)a * (int32_t)b + (1 << 14)) >> 15)
```

This is the scalar form of `_mm256_mulhrs_epi16` (x86 VPMULHRSW) and
ARM `vqrdmulhq_s16` — single-instruction on every relevant target.
The `+ 2^14` is round-to-nearest; the >> 15 strips the Q1.15 scale.

This primitive can only represent factors with `|c| < 1.0`. Any
coefficient with `|c| ≥ 1.0` must be decomposed (see §3).

## 2. Standard coefficient values

From T.800 Annex F, the 9/7 analysis lift uses:

| symbol | value |
|---|---|
| α | −1.586134342 |
| β | −0.052980118 |
| γ | +0.882911075 |
| δ | +0.443506852 |
| K  | +1.230174105 |
| 1/K | +0.812893066 |

For inverse (synthesis), the step order reverses and each coefficient
negates. Since α and β are negative, their synthesis becomes addition.

The Q1.15 encoding of a real value c with `|c| < 1` is
`round(c × 2^15)`. The encodings used below are mechanical
consequences of the standard values:

| name | source value | Q1.15 encoding |
|---|---|---|
| δ direct | 0.443506852 | 14533 |
| γ direct | 0.882911075 | 28931 |
| β boosted (×8) | 0.052980118 × 8 = 0.42384… | 13890 (= round(β × 2^18)) |
| α fractional | 0.586134342 (= |α| − 1) | 19205 |
| (K − 1) fractional | 0.230174105 | 7542 |
| (2/K − 1) fractional | 0.625732422 | 20506 |

Per the cleanroom README, these encodings are mathematical facts and
not Grok-specific tuning. Re-derivation in the implementer's code is
encouraged for clarity but yields the same numbers.

## 3. Per-step decomposition strategies

The four synthesis steps each use a different strategy:

### Step 3 — undo δ (sum-first)
`S[n] -= opj_q15_mul(D[n-1] + D[n], δ_q15)`
Direct Q1.15. Neighbors are summed in int16 before the multiply.

### Step 2 — undo γ (sum-first)
`D[n] -= opj_q15_mul(S[n-1] + S[n+1], γ_q15)`
Same pattern as step 3.

### Step 1 — undo β (multiply-first, precision-boosted)
β = 0.053 is small. Direct Q1.15 encoding wastes ~4 bits of
magnitude. Two consequences:

- **Precision boost:** encode β × 8 instead of β. Multiply each
  neighbor separately, sum the two products, add rounding offset
  4, then right-shift by 3 to cancel the ×8:
  `S[n] += (opj_q15_mul(D[n-1], β8_q15) + opj_q15_mul(D[n], β8_q15) + 4) >> 3`
- **Multiply-first:** because each neighbor is multiplied
  separately, no int16 sum-overflow risk at this step. This step
  is *not* counted in BIBO sum-first headroom (§4).

### Step 0 — undo α (additive decomposition + sum-first)
|α| = 1.586 > 1, so direct Q1.15 is impossible. Decompose as
`|α| = 1 + (|α| − 1) = 1 + 0.586`:

```
sum  = S[n-1] + S[n+1]              # int16
frac = opj_q15_mul(sum, α_frac_q15)
D[n] = D[n] + sum + frac
```

This is sum-first (the sum is formed once and used twice).

### K and 1/K scaling — fused into interleave
`x × K   = x + opj_q15_mul(x, (K − 1)_q15)`
`x × 2/K = x + opj_q15_mul(x, (2/K − 1)_q15)`

Applied while gathering each subband row/column from the strided
band buffer into the contiguous scratch buffer (even samples scaled
by K, odd by 2/K). No separate scaling pass.

## 4. BIBO headroom and downshift

The 9/7 has remarkably low BIBO gain. Cumulative 1D gains from
subband to reconstructed pixel:

```
gain_9x7_l[]:  1.000, 1.380, 1.333, 1.307, 1.303, 1.300, 1.299, …
gain_9x7_h[]:  1.298, 1.313, 1.276, 1.235, 1.231, 1.229, 1.228, …
```

2D worst case (level 1): `1.380² ≈ 1.90` — only ~1 bit of growth.
Sum-first steps (3, 2, 0) momentarily double the input magnitude at
the intermediate sum, doubling the worst-case transient.

**Headroom downshift mechanism:** if the worst-case intermediate
across all levels would exceed ~95% of int16 (±32767), the subband
inputs are right-shifted by `headroom_downshift` bits during the
interleave phase. After the final synthesis level, output is
left-shifted by the same amount before DC shift / clamp. The
precision loss is 1–2 LSBs per shift step, negligible against
quantization noise for lossy.

Per-precision downshift requirement:

| input precision | headroom_downshift |
|---|---|
| 8-bit  | 0 |
| 10-bit | 0 |
| 12-bit | 0–1 |
| 14-bit | 1–2 |

At 8-bit (the primary openjp2k target slice), the mechanism is
dormant — no shift on the way in, no shift on the way out. The
plumbing must still exist if the path is gated to 12-bit, but at
8-bit it's a no-op.

## 5. Eligibility gates

Grok selects the 16-bit 9/7 path at runtime when all hold:

1. Irreversible wavelet (qmfbid == 0).
2. Whole-tile decoding — no region-of-interest partial decode.
3. Image precision ≤ 12 bits.

Same shape as our SP-3.2 5/3 gates. (3) maps to our existing
`tcd.c` precision-check pattern; (2) maps to the
`whole_tile_decoding` flag we already key on.

## 6. Scalar 1D synthesis structure

The horizontal and vertical scalar kernels share this skeleton
(written here from scratch — *not* a transcription):

```
1. INTERLEAVE phase:
   For i in [0, height):
     if (i + parity) even:
       scratch[i] = apply_K_scale(bandL[lIdx * strideL]); lIdx++
     else:
       scratch[i] = apply_invK_scale(bandH[hIdx * strideH]); hIdx++

2. STEP 3 (undo δ): every even position
   For i = parity, parity+2, … < height:
     d_prev = (i > 0)            ? scratch[i-1] : scratch[i+1]
     d_next = (i + 1 < height)   ? scratch[i+1] : scratch[i-1]
     scratch[i] -= opj_q15_mul(d_prev + d_next, δ_q15)

3. STEP 2 (undo γ): every odd position — same shape as Step 3,
   using γ_q15.

4. STEP 1 (undo β, precision-boosted, multiply-first):
   For i = parity, parity+2, … < height:
     d_prev / d_next as above
     prod = opj_q15_mul(d_prev, β8_q15) + opj_q15_mul(d_next, β8_q15) + 4
     scratch[i] += (prod >> 3)

5. STEP 0 (undo α, additive): every odd position
   For i = 1-parity, …  < height:
     s_prev / s_next as above
     sum  = s_prev + s_next
     frac = opj_q15_mul(sum, α_frac_q15)
     scratch[i] += sum + frac

6. SCATTER phase: write scratch[i] back to dest[i * strideDest]
   for i in [0, height).
```

Edge handling is symmetric mirror: when `i == 0`, the "previous"
neighbor is `scratch[i+1]`; when `i+1 == height`, the "next"
neighbor is `scratch[i-1]`. Special-case `height == 1`: emit a
single sample scaled by K or 2/K according to parity.

`parity` is the parity of the first output row/column index — i.e.
whether output row 0 is even (low-pass) or odd (high-pass) in the
subband decomposition.

## 7. Final-level DC shift and clamp

At the deepest (final) synthesis resolution only, the scatter phase
is replaced by a fused DC-shift + clamp:

```
pixel = clamp(synth_value + dc_shift, dc_min, dc_max)
```

For N-bit unsigned input: `dc_shift = 2^(N-1)`, `dc_min = 0`,
`dc_max = 2^N - 1`. For N ≤ 12, all three constants and the result
fit in int16, so the entire pipeline stays in int16 lanes until the
final output store. Output store widens int16 → int32 into the
component's pixel buffer.

For signed input, `dc_shift = 0` and `dc_min`/`dc_max` are the
signed range. We do not currently target signed input for this path.

## 8. SIMD notes

Grok's vertical pass is scalar; only the horizontal pass is SIMD.
Their header notes multi-column vertical SIMD as "future revision".
The Q1.15 multiply uses Highway's `MulFixedPoint15` which maps to
`VPMULHRSW` (x86), `VQRDMULH` (NEON), `i16x8.q15mulr_sat_s` (WASM),
and the equivalent rounding multiply on RISC-V V.

For an AVX2 implementation in openjp2k:
- 16 lanes of int16 per `__m256i` (vs 8 lanes int32 in the float
  path) — doubles per-vector throughput on horizontal rows.
- `_mm256_mulhrs_epi16` is the direct equivalent of `opj_q15_mul`.
- Sum-first steps require care: the int16 add `_mm256_add_epi16`
  saturates by default in some intrinsics but the unsaturated
  add is what we want here; the headroom analysis ensures no
  overflow in the worst case.

Vertical SIMD is feasible because each multi-column slab of
columns can be processed in parallel (one int16 lane = one column).
The scratch buffer becomes a `lanes × height` strip. This is a
larger surface than horizontal-only SIMD, but the implementation
shape is the same kernel — just with the strided gather replaced by
a SIMD-width contiguous load and the scalar steps replaced by lane-
wise vector ops.

## 9. What this resolves in the SP-3.5 spec

The Q-format in the SP-3.5 design doc was Q5.11 with a planned
Q2.14 multiply path. The cleanroom evidence is that Q1.15 with
additive decomposition for any |c| > 1 is the documented, working
approach. The implications for the spec revision:

- Drop Q5.11; use Q1.15 throughout intermediate state.
- Drop the `MULHRS_Q14` macro entirely. Replace with a direct
  `_mm256_mulhrs_epi16` wrapper named per openjp2k convention.
- Adopt additive decomposition for α, K, 2/K. Adopt the ×8/>>3
  precision boost for β.
- Add the headroom-downshift mechanism with BIBO derivation. At
  the 8-bit slice it's dormant but the plumbing is required for
  the 12-bit gate.
- Edge handling: symmetric mirror as in §6 above. Special-case
  `height == 1`.
- Final-level DC shift + clamp fused into the scatter phase.

The PSNR ≥ 60 dB target is consistent with grok's own
documentation note that the int16 9/7 trades "~0.5 dB peak SNR"
for the speed and memory benefits — i.e. PSNR is *much* higher
than 60 dB on typical content. 60 dB remains an appropriate
hard floor for our verification.
