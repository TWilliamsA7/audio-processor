# Implementation Checklist

Companion to `PROJECT_PLAN.md` (vision/architecture/decisions). This
document is the actionable task list. Check items off as completed.
Every module task follows the same definition of done:

**Module Definition of Done (DoD):**
1. RTL implemented, parameterized, no hardcoded widths — derived via
   `fp_pkg` or explicit parameters.
2. `.f` filelist created/updated, includes `pkg.f`/`interfaces.f` as
   needed, follows existing nesting convention.
3. Unit testbench harness (`tb/unit/<module>/<module>_harness.sv`) +
   C++ driver (`tb.cpp`) with an independent golden model (not a copy
   of the RTL's logic).
4. Per-module `Makefile` added, included in top-level `make test`.
5. Test passes (`make test TARGET=<module>` and full `make test`).
6. No hardcoded widths remain anywhere touched in this task.
7. If the module reuses/extends an existing primitive (`delay_line`,
   `fp_pkg`), confirm no regression in that primitive's own unit test.

Items below only restate DoD steps where a task needs something
*beyond* the default — otherwise "Follow Module DoD" is sufficient.

---

## Track A — DSP Pipeline

### Phase 1 — Shared fixed-point package + primitives (in progress)

- [x] `fp_pkg.sv` created (`mult_width`, `round_width`,
      `shifted_result_width`, `round_half_up`, `saturate`, `is_overflow`)
- [x] `volume_ctrl.sv` refactored to use `fp_pkg::mult_width`
- [x] `saturator.sv` refactored to use `fp_pkg::saturate`
- [x] `pkg.f` filelist created and wired into `volume_ctrl.f`,
      `saturator.f`, `top.f`

- [x] **`fp_pkg` unit testbench**
  - Requirement: cover `saturate`, `round_half_up`, `is_overflow`
    against an independent C++ golden model (re-derive the math in
    C++, don't port the SV logic).
  - Test cases required: in-range value, exact positive boundary,
    exact negative boundary, positive overflow (small + large),
    negative overflow (small + large), zero, `narrow_width` at a
    non-default value (catch width-dependent bugs the way the
    `volume_ctrl` bug was caught).
  - Since these are pure functions (no clock), decide and document
    the harness approach: a thin wrapper module exposing the
    functions via combinational ports is simplest given the existing
    Verilator `--cc`/`--exe` flow.
  - Deliverable: `tb/unit/fp_pkg/`, `rtl/filelists/fp_pkg.f` (if not
    already implied by `pkg.f`).
  - Acceptance: all cases pass; explicitly confirm `round_half_up` and
    `saturate` are validated at a `narrow_width`/`frac_bits` other
    than what `volume_ctrl`/`saturator` currently use.

- [x] **Phase 1 closeout gate**
  - Requirement: grep/review every module touched in Phase 1 for
    hardcoded width literals; confirm `pkg.f` ordering is correct
    everywhere it's included.
  - Acceptance: explicit sign-off note added to `PROJECT_PLAN.md`
    Decision Log before Phase 2 work starts.

### Phase 2 — Filters (biquad, FIR, EQ)

- [x] **Design decision: biquad topology** (Direct Form I vs II
      Transposed) — document choice and rationale before implementing.
- [x] **Design decision: coefficient format** — fixed Q-format vs.
      runtime-configurable; how coefficients enter the module
      (parameter vs. register interface — ties into Phase 6's register
      file, decide the interface shape now even if the register file
      itself doesn't exist yet).
- [x] `biquad.sv` — Follow Module DoD.
  - Additional requirement: golden model in C++ must implement the
    difference equation independently (not derived from the SV), and
    test at minimum: unity/passthrough coefficients, a known
    low-pass/high-pass coefficient set, and a coefficient set that
    exercises saturation via `fp_pkg::saturate`.
- [ ] `fir.sv` — Follow Module DoD.
  - Additional requirement: parameterizable tap count; golden model
    is a direct convolution in C++; test with an odd and even tap
    count, and a tap count of 1 (degenerate case).
- [ ] `eq.sv` (cascaded biquad stages, if this is how EQ is
      structured) — Follow Module DoD.
  - Additional requirement: integration test instantiating multiple
    `biquad` stages; confirm latency through the cascade matches
    `PROC_LATENCY`-style accounting used in `audio_mux`.
- [ ] **Phase 2 closeout gate** — same shape as Phase 1's: hardcoded
      width review + Decision Log entry before Phase 3.

### Phase 3 — Dynamics (compressor, gate, envelope, meter)

- [ ] **Design decision: envelope follower topology** (attack/release
      time constants, log vs. linear domain) — document before coding,
      since compressor/gate/meter all likely depend on it.
- [ ] `envelope.sv` — Follow Module DoD.
- [ ] `compressor.sv` — Follow Module DoD.
  - Additional requirement: golden model needs a gain-computer +
    envelope-follower reference implemented independently in C++;
    test threshold/ratio/attack/release combinations, plus a case at
    the saturation boundary.
- [ ] `gate.sv` — Follow Module DoD.
- [ ] `meter.sv` — Follow Module DoD.
  - Note: likely the first module without an audio-out path (metadata
    only) — confirm interface shape (new interface type, or repurpose
    existing) before implementing.
- [ ] **Phase 3 closeout gate**.

### Phase 4 — Time-based FX (delay, chorus, reverb, LFO)

- [ ] **Design decision: memory strategy for long delay lines** —
      existing `delay_line` is register-based (fine for Phase 0-3's
      short latencies); reverb-length delays likely need BRAM-inferred
      storage. Decide and document the approach (new module vs.
      `delay_line` extension) before implementing.
- [ ] `lfo.sv` — Follow Module DoD.
- [ ] `delay_fx.sv` (feedback delay, distinct from the `delay_line`
      primitive) — Follow Module DoD.
- [ ] `chorus.sv` — Follow Module DoD.
- [ ] `reverb.sv` — Follow Module DoD.
  - Additional requirement: given complexity, golden model should be
    built and validated incrementally (single comb filter → single
    all-pass → full network) rather than as one large C++ reference.
- [ ] **Phase 4 closeout gate**.

### Phase 5 — Multiband / FFT

- [ ] **Design decision: FFT approach** — custom RTL FFT vs. a
      known open IP core vs. polyphase filterbank instead of true FFT.
      This decision has significant scope impact; document the
      tradeoff explicitly before committing.
- [ ] (Tasks depend on above decision — expand this section once the
      FFT approach is chosen. Do not begin implementation before this
      decision is documented in `PROJECT_PLAN.md`.)
- [ ] **Phase 5 closeout gate**.

### Phase 6 — System integration

- [ ] **Design decision: register file interface** — memory-mapped
      (APB/AXI-lite style) vs. custom parallel bus; must be decided
      before Phase 2's coefficient-loading approach is finalized
      retroactively, or accepted as a Phase 6 refactor.
- [ ] `register_file.sv` — Follow Module DoD.
- [ ] `routing_matrix.sv` — Follow Module DoD.
- [ ] Stereo support — requires auditing every Track A module for
      mono-only assumptions; document findings before implementing.
- [ ] I2S interface module — Follow Module DoD.
  - Additional requirement: this is the first module with a real
    external timing interface; testbench needs a bit-clock/frame-sync
    generator model, not just a sample-valid handshake.
- [ ] **Full-system integration test** — extend `tb/tb.cpp`-style
      WAV-driven test to exercise the complete Phase 0-6 signal chain.
- [ ] **Phase 6 closeout gate.**

---

## Track B — NN Accelerator (RVC NSF-HiFiGAN vocoder, 40k config)

### NN-Phase 0 — Golden model foundation

- [ ] **Obtain a fine-tuned or pretrained RVC 40k checkpoint.**
  - Acceptance: checkpoint loads in a reference PyTorch environment;
    `dec.*` (generator) weight shapes match §4.3 of `PROJECT_PLAN.md`.
- [ ] **Extract real intermediate activations** from the reference
      model for at least one short input sequence — pre-conv output,
      each upsampling stage output, each resblock output, final
      waveform. Save as fixture data (e.g. `.npy`/`.csv`) for later
      RTL-vs-golden comparison.
  - Acceptance: fixture files committed (or documented external
    location, if too large for the repo) and reproducible via a
    checked-in extraction script.
- [ ] **NumPy/pure-Python reference implementation of a single conv1d
      layer** (not using PyTorch's `nn.Conv1d` internals — a from-scratch
      reference is needed so it can be ported to C++ later without a
      framework dependency).
  - Acceptance: reference conv1d output matches the extracted PyTorch
    activation for a known layer within acceptable numerical tolerance
    (document tolerance and why).
- [ ] **Reference implementation of ConvTranspose1d**, same
      requirement as above.
- [ ] **Reference implementation of dilated Conv1d**, same requirement.
- [ ] **Quantization scheme decision** (int8 vs int16 weights/activations).
  - Requirement: document accumulator width implications using
    `fp_pkg::mult_width`-style reasoning; quantize the reference model
    and confirm converted-audio quality is still acceptable
    (subjective listening check is fine at this stage — this is a
    sanity check, not a formal metric).
  - Acceptance: decision + rationale recorded in `PROJECT_PLAN.md`
    Decision Log before NN-1 starts.

### NN-Phase 1 — Single PE (processing element)

- [ ] **Design decision: PE datapath** — MAC width, accumulator width
      (derived from quantization decision above via `fp_pkg`-style
      width functions), whether to add a new `nn_pkg.sv` or extend
      `fp_pkg`.
- [ ] `pe.sv` (single MAC unit with accumulate/hold/clear control) —
      Follow Module DoD.
  - Golden model: scalar MAC sequence in C++, matching chosen
    quantization scheme exactly (including rounding behavior).
- [ ] **Activation LUT/function module** (LeakyReLU, slope 0.1 per
      RVC's config) — Follow Module DoD.
  - Acceptance: bit-exact or documented-tolerance match against the
    quantized reference activation function.
- [ ] **NN-Phase 1 closeout gate.**

### NN-Phase 2 — Systolic array + behavioral weight memory

- [ ] **Design decision: dataflow** (weight-stationary recommended per
      `PROJECT_PLAN.md` §4) and array dimensions (rows × cols) —
      document rationale, including how this maps onto the channel
      widths in §4.3 (512/256/128/64/32).
- [ ] **Behavioral weight memory model** in the C++ testbench harness
      (address/data or streaming interface standing in for DDR).
  - Requirement: interface contract (burst size, addressing scheme,
    handshake signals) documented explicitly — this is the boundary
    that gets swapped for a real memory controller later, so it needs
    to be treated as a stable spec, not an implementation detail.
- [ ] `pe_array.sv` (generate-block array of `pe` instances, in the
      style of `delay_line`'s generate pattern) — Follow Module DoD.
- [ ] **NN-Phase 2 closeout gate.**

### NN-Phase 3 — One full layer, verified against real activations

- [ ] **Weight-loading sequencer** (streams one layer's weights from
      behavioral memory into the array) — Follow Module DoD.
- [ ] **End-to-end single-layer test**: weight load → array compute →
      activation → writeback, using the *actual* fine-tuned weights and
      the *actual* extracted activations from NN-Phase 0 as the
      expected output (not synthetic vectors).
  - Acceptance: RTL output matches the golden/extracted activation
    within the documented quantization tolerance, for the pre-conv
    layer specifically (simplest layer: plain Conv1d, kernel 7).
- [ ] **NN-Phase 3 closeout gate.**

### NN-Phase 4 — Dilated conv + transposed conv + full resblock/stage

- [ ] `conv1d_transpose.sv` — Follow Module DoD; verify against a real
      extracted upsampling-stage activation (Stage 1: 512→256, stride
      10, kernel 16).
- [ ] **Dilated conv support** added to the layer sequencer (dilation
      1/3/5, programmable) — Follow Module DoD; verify against a real
      extracted resblock activation.
- [ ] `resblock.sv` (3 dilated-conv pairs + residual add, per
      `ResBlock1`) — Follow Module DoD.
- [ ] `mrf_block.sv` (3 parallel resblocks summed) — Follow Module DoD.
- [ ] **One full upsampling stage integration test** (transpose conv +
      MRF block), verified against real extracted Stage 1 output.
- [ ] **NN-Phase 4 closeout gate.**

### NN-Phase 5 — Full generator chain

- [ ] Chain all 4 upsampling stages + MRF blocks.
- [ ] Speaker-conditioning 1×1 conv (`gin_channels = 256`) integration
      — Follow Module DoD.
- [ ] NSF source/excitation branch (`noise_convs`) — Follow Module DoD.
  - Design decision needed: how the sine-excitation signal itself is
    generated (host-computed and streamed in vs. on-FPGA) — document
    before implementing.
- [ ] Post-conv + tanh → waveform output — Follow Module DoD.
- [ ] **Full-generator end-to-end test**: real fine-tuned weights, real
      input features, RTL waveform output compared against the
      reference PyTorch model's waveform output (objective metric,
      e.g. max sample error or SNR against reference — define
      threshold before running the test).
- [ ] **NN-Phase 5 closeout gate.**

### NN-Phase 6 — Host↔FPGA integration

- [ ] Resolve open design questions in `PROJECT_PLAN.md` §5 (chunk
      size/buffering, transport protocol, speaker-embedding
      load timing) — document decisions before implementing.
- [ ] Host-side feature-streaming module/script (Python is fine here —
      this doesn't need to be RTL).
- [ ] FPGA-side stream ingest module — Follow Module DoD.
- [ ] Integration with Track A's `audio_stream_if`-style output path.
- [ ] **End-to-end latency measurement** against the target established
      in `PROJECT_PLAN.md` §5.
- [ ] **NN-Phase 6 closeout gate.**

---

## Deferred / Not Yet Scoped

Items intentionally left out of the checklist above because a
prerequisite decision hasn't been made. Move into the relevant phase
section once unblocked.

- [ ] Real FPGA board selection and resource budget (blocks: real DDR
      controller work, final PE array sizing, Track A synthesis
      targets)
- [ ] Whether VITS posterior encoder/flow decoder ever moves to FPGA
      (currently host-side indefinitely per `PROJECT_PLAN.md` §4.2)
- [ ] Track A / Track B shared clock domain and integration point at
      the top level (`top.sv` currently only reflects Track A)