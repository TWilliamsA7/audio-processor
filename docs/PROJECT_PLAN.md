# SystemVerilog Audio Processing Suite + FPGA Voice-Conversion Accelerator

**Status:** Living document — update as phases complete or decisions change.
**Last updated:** 2026-08-28

---

## 1. Vision

A modular, testable SystemVerilog audio processing suite targeting hardware
modules, which extends beyond conventional DSP into a real-time voice
conversion system: an FPGA-accelerated neural network runs the
latency-critical half of a voice-conversion pipeline, fed by content/pitch
features extracted on a host device from a live audio input, so a spoken
voice can be converted to a target (cloned) voice in real time.

The project has two major tracks that converge at system integration:

- **DSP Track** — the original six-phase roadmap: gain, saturation, filters,
  dynamics, time-based FX, multiband/FFT, and system integration
  (register file, routing, I2S).
- **NN Accelerator Track** — a custom FPGA-based neural network accelerator
  that runs the real-time-critical portion of a voice-conversion model.

These tracks share design philosophy (phase gates, per-module testbenches,
no hardcoded widths, system-level thinking from day one) and will eventually
share the audio I/O and streaming infrastructure built in the DSP track.

---

## 2. Core Design Principles

- No module is designed in isolation — every implementation decision
  considers system-level integration.
- Every module requires a matching testbench.
- No hardcoded width values survive past a phase boundary; widths are
  derived via `fp_pkg` functions or explicit parameters.
- Work does not advance to the next phase until the current phase's
  foundation is solid (no unverified load-bearing functions, no known
  latent bugs).
- Pull work forward from a later phase when deferring it increases risk
  (e.g. `saturator.sv` was pulled from Phase 2 into Phase 1 because of its
  `fp_pkg` dependency).
- Hand-trace verification is used when tooling can't be run directly;
  actual hardware/Verilator runs must be confirmed by the project owner.

---

## 3. Track A — DSP Pipeline Roadmap

| Phase | Scope | Status |
|-------|-------|--------|
| 0 | Latency/glitch fixes and foundation | Complete |
| 1 | Shared fixed-point package (`fp_pkg`) + reusable primitives (`delay_line`, `saturate`) | In progress |
| 2 | Filters (biquad, FIR, EQ) | Not started |
| 3 | Dynamics (compressor, gate, envelope, meter) | Not started |
| 4 | Time-based FX (delay, chorus, reverb, LFO) | Not started |
| 5 | Multiband / FFT | Not started |
| 6 | System integration (register file, routing matrix, stereo, I2S) | Not started |

### 3.1 Phase 1 detail (current)

Completed:
- `fp_pkg.sv` (`rtl/pkg/fp_pkg.sv`) — width calculation functions
  (`mult_width`, `round_width`, `shifted_result_width`) and arithmetic
  helpers (`round_half_up`, `saturate`, `is_overflow`). Functions use
  `logic signed [63:0]` signatures (SV packages cannot take parameters);
  callers sign-extend inputs and slice outputs.
- `volume_ctrl.sv` refactored to derive `PRODUCT_WIDTH` via
  `fp_pkg::mult_width`, fixing a latent bug where hardcoded casts were
  only correct at default parameterizations.
- `saturator.sv` refactored to use `fp_pkg::saturate`, with a documented
  equivalence proof against the original sign-bit comparison approach.
- Filelist infrastructure: `rtl/filelists/pkg.f` added; `volume_ctrl.f`
  and `saturator.f` updated to include it; `pkg.f` precedes importing
  modules in `top.f`.

Remaining before Phase 1 closes:
- Dedicated `fp_pkg` unit testbench covering `saturate`, `round_half_up`,
  and `is_overflow` against a C++ golden model.
- Confirm all Phase 1 primitives are settled before Phase 2 begins.

---

## 4. Track B — NN Accelerator Roadmap

### 4.1 Key architecture decision: Option 3 (split pipeline)

Three architecture options were evaluated for bringing neural
voice-conversion onto the FPGA:

| | Option 1: Fully on-FPGA, conv/GRU | Option 2: Fully on-FPGA, transformer | **Option 3 (chosen): Split** |
|---|---|---|---|
| Hardware scope | Entire pipeline | Entire pipeline incl. attention | Only decoder + vocoder |
| Complexity | Lowest | Highest (softmax, score buffers, memory-bound attention traffic) | Same as Option 1's hardware, plus a host↔FPGA feature-streaming interface |
| Quality ceiling | Lower (older architectures) | Highest | Matches Option 2 — encoder can be a full transformer, just not on-FPGA |
| Attention in hardware | No | Yes | Never |
| Risk | Low | High (attention subsystem could stall the project) | Low — attention is permanently out of scope |

**Decision:** Build the FPGA accelerator to the scope of Option 1
(conv/linear/activation only, no attention hardware, ever), and adopt
Option 3's system split: the attention-heavy content encoder runs on a
host device; only the real-time-critical decoder/vocoder runs on the
FPGA. This makes the FPGA an **edge acceleration device** for the
voice-conversion application rather than a full end-to-end NN host.

### 4.2 Target model: RVC (Retrieval-based Voice Conversion)

- **Source:** `RVC-Project/Retrieval-based-Voice-Conversion-WebUI`, MIT
  licensed, actively maintained, fine-tunable from as little as ~10
  minutes of clean speech.
- **Plan:** fine-tune an existing pretrained RVC checkpoint rather than
  training a voice-conversion model from scratch. Model design/training
  is intentionally out of scope for this project — the focus is the
  hardware accelerator.
- **Sample rate config: 40k** (locked in). This determines the exact
  generator shapes used for NN-Phase 0 onward (see §4.3).

RVC pipeline components and where they run:

| Component | Role | Where it runs |
|---|---|---|
| HuBERT content encoder (~95M params, transformer) | Extracts phonetic/content features from source audio | **Host** (out of scope for FPGA) |
| FAISS feature retrieval | Blends content features with nearest training-set features to reduce source-speaker tone leakage | **Host** |
| RMVPE pitch (F0) extraction | Extracts pitch curve | **Host** |
| VITS posterior encoder / flow-based decoder | Conv/WaveNet-style acoustic modeling | **Host** (bundled with encoder stage for now — revisit if it becomes a bottleneck) |
| **NSF-HiFiGAN generator (vocoder)** | Converts features + F0 → waveform | **FPGA — this is the accelerator's target workload** |

### 4.3 NN-Phase 0 golden-model target: NSF-HiFiGAN generator (40k config)

Confirmed from RVC's actual shipped 40k training config.

- **Input:** `initial_channel = 192` (RVC's `inter_channels`, output of the
  host-side flow decoder)
- **Pre-conv:** `Conv1d(192 → 512, kernel=7, pad=3)`
- **4 upsampling stages** (`upsample_rates = [10, 10, 2, 2]`, total 400x,
  matching hop length):

  | Stage | Type | Kernel | Channels in → out |
  |---|---|---|---|
  | 1 | ConvTranspose1d, stride 10 | 16 | 512 → 256 |
  | 2 | ConvTranspose1d, stride 10 | 16 | 256 → 128 |
  | 3 | ConvTranspose1d, stride 2 | 4 | 128 → 64 |
  | 4 | ConvTranspose1d, stride 2 | 4 | 64 → 32 |

- **MRF block after each upsampling stage:** 3 parallel `ResBlock1`s summed,
  kernel sizes `[3, 7, 11]`, dilations `[1, 3, 5]` each. Each `ResBlock1` is
  3 dilated-conv pairs (dilated conv → LeakyReLU → conv → residual add) —
  18 conv1d layers per stage.
- **Speaker conditioning:** `gin_channels = 256`, injected via a single 1×1
  conv (not per-layer).
- **NSF source branch:** sine-wave excitation from the F0 curve, merged in
  via small `noise_convs` per upsampling stage.
- **Post-conv:** `Conv1d(32 → 1, kernel=7, pad=3)` + tanh → waveform sample.

**Required op set (confirmed, final — no attention anywhere in this
submodule):**
- `Conv1dTranspose` (strided upsampling)
- `Conv1d`, including **dilated** variants (dilation 1/3/5, programmable)
- LeakyReLU
- Elementwise add (residual/skip, MRF summation)
- One 1×1 conv (speaker conditioning)

**Weight footprint:** low-single-digit millions of parameters, dominated
by the two stride-10/kernel-16 transposed convs. Expected to be a handful
of MB at int8 — external memory budget should be modest on any
dev-kit-class FPGA.

### 4.4 NN accelerator phase structure (parallel to Track A, not blocking it)

| Phase | Scope |
|-------|-------|
| NN-0 | Confirm target network (done — NSF-HiFiGAN, 40k config, §4.3). Build NumPy/PyTorch golden model for a single conv1d/transposed-conv layer, validated against a fine-tuned RVC checkpoint's actual weights/activations. |
| NN-1 | Single MAC/PE design + accumulator, unit-tested against golden model (mirrors `fp_pkg` unit-test discipline). Quantization scheme (int8 vs int16) decided here — sets accumulator width, reuses `fp_pkg` rounding/saturation primitives. |
| NN-2 | Systolic array assembly (generate-block array of PEs, in the spirit of `delay_line`'s generate pattern), weight-stationary dataflow, **behavioral weight memory** in the C++ testbench harness standing in for external DDR. |
| NN-3 | One full layer end-to-end (weight streaming → array → activation LUT → writeback), verified layer-by-layer against real intermediate activations from the fine-tuned model, not synthetic vectors. |
| NN-4 | Chain multiple layers; add dilated-conv and transposed-conv support; assemble a full upsampling stage + MRF block. |
| NN-5 | Full generator chain (all 4 stages + pre/post conv + speaker conditioning + NSF source branch). |
| NN-6 | Host↔FPGA streaming interface integration (see §5). Real memory controller swapped in once an actual FPGA board is selected. |

**Explicitly out of scope, permanently:** attention/softmax hardware, the
HuBERT content encoder, FAISS retrieval, and model training itself.

---

## 5. Host ↔ FPGA Interface (design surface — not yet detailed)

The FPGA accelerator receives, per audio chunk:
- Content/acoustic features (host-side encoder + retrieval output,
  `inter_channels = 192` width)
- F0 (pitch) curve (RMVPE, host-side)
- Speaker embedding (`gin_channels = 256`, likely static per session)

And streams back:
- Raw waveform samples, into the existing `audio_stream_if`-style
  pipeline built in Track A.

Open design questions (to resolve before NN-6):
- Chunk size / buffering strategy and its contribution to end-to-end
  latency (real-world RVC real-time inference is commonly ~100–300ms
  end-to-end even in pure software; the FPGA vocoder path should aim to
  not be the dominant contributor).
- Transport: AXI-Stream vs. a custom FIFO-based handshake.
- Whether speaker embedding is loaded once per session vs. streamed per
  chunk.

---

## 6. Target Hardware

- **Current target:** Verilator simulation only. No FPGA board selected
  yet.
- **FPGA board and budget:** deferred until the design (both tracks) is
  further along. Memory model for NN accelerator work is a behavioral
  stand-in in the C++ testbench harness until a board is chosen; the
  host-facing interface (§5) is being designed so the behavioral model
  can be swapped for a real DDR/AXI controller without changing the
  RTL-side contract.

---

## 7. Tools & Repo Conventions

- **HDL:** SystemVerilog
- **Simulation/verification:** Verilator, classic `--cc`/`--exe` C++-driven
  flow (SV `--timing` mode found unreliable with SV interfaces and
  generate-block arrays in this environment).
- **Build system:** GNU Make, shared `tb/unit/unit.mk` fragment with
  inherited rules, per-module minimal Makefiles, top-level `make test`.
  Two-variable convention: `F_FILES` for `.f` filelists, `SV_SRCS` for
  plain `.sv` files.
- **Repo structure:**
  - `rtl/pkg/` — shared packages (`fp_pkg.sv`)
  - `rtl/interfaces/` — SV interfaces (`audio_stream_if`, `wide_stream_if`)
  - `rtl/filelists/` — per-module `.f` files with nested `-f` includes;
    `rtl/filelists/interfaces.f` shared by all; `rtl/filelists/top.f`
    reserved for the top-level system build only (unit tests never
    reference it)
  - `tb/unit/<module>/` — per-module testbench directories
  - `tb/unit/.obj/` — build artifacts
- **Verilator path resolution:** all invocations `cd $(ROOT)` first so
  filelist paths resolve from repo root regardless of invocation
  directory.
- **SV package constraint:** packages cannot take parameters; established
  pattern is `logic signed [63:0]` function signatures with caller-side
  sign-extension and output slicing (see `fp_pkg`).
- **Interface design as anti-refactor strategy:** interfaces are split
  for the full system's needs, not just the current pair of modules
  (e.g. `audio_stream_if` vs. `wide_stream_if`).
- **Development environment:** WSL.

---

## 8. Open Questions / TODO

- [ ] Finalize Phase 1: `fp_pkg` unit testbench against C++ golden model
- [ ] Pick int8 vs. int16 quantization for the NN accelerator (NN-1)
- [ ] Obtain a fine-tuned RVC 40k checkpoint to use as the real
      golden-model weight source for NN-0/NN-3
- [ ] Design the host↔FPGA chunk/streaming interface in detail (§5)
- [ ] Select target FPGA board and real budget once design is mature
      enough to estimate resource needs
- [ ] Decide whether the VITS posterior encoder/flow decoder ever moves
      to FPGA, or stays host-side indefinitely alongside HuBERT

---

## 9. Decision Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-08-28 | Adopted Option 3 split architecture (encoder host-side, decoder/vocoder FPGA-side) | Avoids attention hardware entirely while preserving state-of-the-art quality; keeps hardware scope bounded to conv/linear/activation ops |
| 2026-08-28 | Target model: RVC, fine-tuned rather than trained from scratch | Keeps ML work bounded; project focus is the hardware accelerator |
| 2026-08-28 | Sample rate config locked to 40k | Matches RVC's most common default; gives concrete, final layer shapes for NN-Phase 0 |
| 2026-08-28 | NN accelerator hardware scope = NSF-HiFiGAN generator only | Fully convolutional, no attention, small parameter count (~low millions), confirmed via actual shipped RVC config |