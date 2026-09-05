#include <cstdio>
#include <cstdint>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>
#include <verilated.h>
#include "Vfir_harness.h"

// ---------------------------------------------------------------------
// Parameters (must match G_PARAMS in the Makefile / fir_harness defaults).
// ---------------------------------------------------------------------
static constexpr int SAMPLE_WIDTH = 24;
static constexpr int COEFF_WIDTH  = 16;
static constexpr int COEFF_FRAC   = 15;   // Q1.15
static constexpr int GUARD_BITS   = 1;
static constexpr int NUM_TAPS     = 8;

static int errors = 0;

// ---------------------------------------------------------------------
// Bit-level helpers
// ---------------------------------------------------------------------

static int64_t sign_extend(uint64_t raw, int width) {
    uint64_t mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
    int64_t v = (int64_t)(raw & mask);
    if (v & (1LL << (width - 1))) v -= (1LL << width);
    return v;
}

static int64_t golden_round_half_up(int64_t value, uint32_t frac_bits) {
    if (frac_bits == 0) return value;
    return value + (1LL << (frac_bits - 1));
}

static int64_t golden_saturate(int64_t value, uint32_t narrow_width) {
    int64_t max_val = (1LL << (narrow_width - 1)) - 1;
    int64_t min_val = -(1LL << (narrow_width - 1));
    if (value > max_val) return max_val;
    if (value < min_val) return min_val;
    return value;
}

static void check(const char* name, int64_t got_data, int got_valid, int64_t exp_data, int exp_valid) {
    if (got_data != exp_data || got_valid != exp_valid) {
        std::printf("FAIL [%s]: expected data=%lld valid=%d, got data=%lld valid=%d\n",
                     name, (long long)exp_data, exp_valid, (long long)got_data, got_valid);
        errors++;
    } else {
        std::printf("PASS [%s]: data=%lld valid=%d\n", name, (long long)got_data, got_valid);
    }
}

// ---------------------------------------------------------------------
// Bit-exact golden model.
//
// Accumulation is computed as an exact full-precision int64_t sum over all
// NUM_TAPS products, independent of any particular addition order. This is
// deliberate, not a simplification: fp_pkg::accum_width sizes ACC_WIDTH so
// that the true mathematical sum (in any grouping) cannot overflow under
// valid Q1.15-coefficient / in-range-sample conditions, so int64_t addition
// (exact, associative) reproduces the hardware result bit-for-bit without
// needing to mirror the RTL's specific left-to-right reduction. This also
// means the test stays valid if the accumulator is later restructured into
// a pipelined adder tree for timing closure -- a structural change that
// isn't a functional one.
// ---------------------------------------------------------------------

struct FirGolden {
    std::vector<int64_t> coeff;   // NUM_TAPS coefficients, Q1.15 as raw int16
    std::vector<int64_t> hist;    // x[n-1]..x[n-(NUM_TAPS-1)], same layout as tapped_delay_line
    int64_t y_reg = 0;
    bool    reg_valid = false;

    FirGolden() : coeff(NUM_TAPS, 0), hist(NUM_TAPS > 1 ? NUM_TAPS - 1 : 0, 0) {}

    void reset() {
        std::fill(coeff.begin(), coeff.end(), 0);
        std::fill(hist.begin(), hist.end(), 0);
        y_reg = 0;
        reg_valid = false;
    }

    // One clock edge. x/valid/we/addr/data are pre-edge port values.
    // Returns post-edge (downstream.data, downstream.valid).
    std::pair<int64_t, bool> tick(int64_t x, bool valid, bool we, int addr, int64_t data) {
        // --- combinational, using PRE-edge coeff_mem and PRE-edge hist --
        // a same-cycle coeff_we must NOT affect this cycle's compute,
        // matching biquad/eq's coeff_load convention. ---
        int64_t acc = 0;
        for (int k = 0; k < NUM_TAPS; k++) {
            int64_t sample_k = (k == 0) ? x : hist[k - 1];
            acc += sample_k * coeff[k];
        }

        int64_t rounded = golden_round_half_up(acc, COEFF_FRAC);
        int64_t shifted = rounded >> COEFF_FRAC;   // arithmetic shift (signed)
        int64_t y_sat   = golden_saturate(shifted, SAMPLE_WIDTH);

        // --- registered updates, all landing on this same edge ---
        reg_valid = valid;
        if (valid) {
            y_reg = y_sat;
            if (NUM_TAPS > 1) {
                for (int i = (int)hist.size() - 1; i >= 1; i--) hist[i] = hist[i - 1];
                hist[0] = x;
            }
        }
        if (we) {
            coeff[addr] = data;
        }

        return { y_reg, reg_valid };
    }
};

// ---------------------------------------------------------------------
// FIR design math (frequency-response validation only). Windowed-sinc
// low-pass, Hamming window -- coefficients come out already causal
// (h[k] multiplies x[n-k] directly), matching coeff_mem[k]'s layout.
// ---------------------------------------------------------------------

static std::vector<double> design_lowpass_fir(int num_taps, double fc_norm) {
    std::vector<double> h(num_taps);
    double M = num_taps - 1;
    for (int n = 0; n < num_taps; n++) {
        double m = n - M / 2.0;
        double sinc = (m == 0.0) ? 2.0 * fc_norm
                                  : std::sin(2.0 * M_PI * fc_norm * m) / (M_PI * m);
        double window = 0.54 - 0.46 * std::cos(2.0 * M_PI * n / M);
        h[n] = sinc * window;
    }
    return h;
}

static int64_t quantize_coeff(double value) {
    int64_t max_val = (1LL << (COEFF_WIDTH - 1)) - 1;
    int64_t min_val = -(1LL << (COEFF_WIDTH - 1));
    int64_t rounded = (int64_t)std::llround(value * double(1LL << COEFF_FRAC));
    if (rounded > max_val) rounded = max_val;
    if (rounded < min_val) rounded = min_val;
    return rounded;
}
static double dequantize_coeff(int64_t raw) {
    return double(raw) / double(1LL << COEFF_FRAC);
}

static double dft_magnitude(const std::vector<double>& h, double f_norm) {
    double re = 0.0, im = 0.0;
    for (size_t n = 0; n < h.size(); n++) {
        double angle = -2.0 * M_PI * f_norm * double(n);
        re += h[n] * std::cos(angle);
        im += h[n] * std::sin(angle);
    }
    return std::sqrt(re * re + im * im);
}
static double to_db(double mag) { return 20.0 * std::log10(std::max(mag, 1e-12)); }

static void check_freq_response(const char* name, double f_norm,
                                 double mag_dut, double mag_ref, double tol_db) {
    double diff = std::abs(to_db(mag_dut) - to_db(mag_ref));
    if (diff > tol_db) {
        std::printf("FAIL [%s] f=%.4f: dut=%.2fdB ref=%.2fdB diff=%.3fdB (tol %.2fdB)\n",
                     name, f_norm, to_db(mag_dut), to_db(mag_ref), diff, tol_db);
        errors++;
    } else {
        std::printf("PASS [%s] f=%.4f: dut=%.2fdB ref=%.2fdB diff=%.3fdB (within %.2fdB)\n",
                     name, f_norm, to_db(mag_dut), to_db(mag_ref), diff, tol_db);
    }
}
static void report_freq_response_info(const char* name, double f_norm,
                                       double mag_dut, double mag_ideal) {
    double diff = std::abs(to_db(mag_dut) - to_db(mag_ideal));
    std::printf("INFO [%s] f=%.4f: dut=%.2fdB ideal=%.2fdB diff=%.3fdB (quantization effect, not a failure)\n",
                name, f_norm, to_db(mag_dut), to_db(mag_ideal), diff);
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto dut = std::make_unique<Vfir_harness>();
    FirGolden golden;

    auto reset_dut = [&]() {
        dut->clk = 0; dut->rst_n = 0;
        dut->in_valid = 0; dut->coeff_we = 0; dut->coeff_addr = 0; dut->coeff_data = 0;
        dut->in_data = 0;
        for (int i = 0; i < 6; i++) {
            dut->clk = !dut->clk;
            if (i == 2) dut->rst_n = 1;
            dut->eval();
        }
        golden.reset();
    };

    auto tick = [&](int64_t x, bool valid, bool we, int addr, int64_t data) -> std::pair<int64_t, bool> {
        dut->clk = 0;
        dut->in_data    = (uint32_t)x & 0xFFFFFFu;
        dut->in_valid   = valid;
        dut->coeff_we   = we;
        dut->coeff_addr = (uint32_t)addr;
        dut->coeff_data = (uint16_t)data;
        dut->eval();
        dut->clk = 1;
        dut->eval();
        return golden.tick(x, valid, we, addr, data);
    };
    auto dut_out = [&]() { return sign_extend(dut->out_data, SAMPLE_WIDTH); };

    // ---- Directed bit-exact cases ----
    reset_dut();
    check("post-reset", dut_out(), dut->out_valid, 0, 0);

    // Load tap 0 = 0.5 exactly (16384 in Q1.15 -- exact, unlike 1.0 which
    // isn't representable in Q1.15), all other taps 0: half-scale passthrough.
    {
        auto e = tick(0, false, true, 0, 16384);
        check("load-tap0-half", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(1000, true, false, 0, 0);
        check("half-scale-sample1", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(-2000, true, false, 0, 0);
        check("half-scale-sample2", dut_out(), dut->out_valid, e.first, e.second);
    }

    // coeff_we on the SAME cycle as a data sample -- that sample must still
    // use the OLD coefficients; new ones apply starting next cycle.
    {
        // switch tap0: 0.5 -> 0.25 (8192), same cycle as a data sample
        auto e = tick(4000, true, true, 0, 8192);
        check("coeff-we-same-cycle-uses-old-coeff", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(4000, true, false, 0, 0);
        check("coeff-we-next-cycle-uses-new-coeff", dut_out(), dut->out_valid, e.first, e.second);
    }

    // valid drop holds state
    {
        auto e = tick(300, true, false, 0, 0);
        check("push-before-drain", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(0, false, false, 0, 0);
        check("valid-drops-holds", dut_out(), dut->out_valid, e.first, e.second);
    }

    // Bubble must not corrupt tap history: push a sequence, bubble, resume,
    // and confirm multi-tap accumulation (tap0 and tap1 both nonzero) stays
    // bit-exact across the gap.
    reset_dut();
    {
        auto e = tick(0, false, true, 0, 16384);   // tap0 = 0.5
        check("multitap-load-tap0", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(0, false, true, 1, 8192);    // tap1 = 0.25
        check("multitap-load-tap1", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(1000, true, false, 0, 0);    // y = 0.5*1000 = 500
        check("multitap-sample1", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        // bubble -- history (x[n-1]=1000) must be held, not disturbed
        auto e = tick(9999, false, false, 0, 0);
        check("multitap-bubble", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        // resume: y = 0.5*2000 + 0.25*1000 = 1000 + 250 = 1250
        // (the held x[n-1]=1000 from before the bubble, not the bubble's 9999)
        auto e = tick(2000, true, false, 0, 0);
        check("multitap-resume-after-bubble", dut_out(), dut->out_valid, e.first, e.second);
    }

    // ---- Frequency response: windowed-sinc low-pass over the full 8-tap
    // filter, all coefficients live and nonzero (unlike the directed cases
    // above, which only exercise 1-2 taps at a time). ----
    auto run_freq_case = [&](const char* name, double fc_norm) {
        std::vector<double> ideal = design_lowpass_fir(NUM_TAPS, fc_norm);
        std::vector<int64_t> q(NUM_TAPS);
        std::vector<double> quant(NUM_TAPS);
        for (int k = 0; k < NUM_TAPS; k++) {
            q[k] = quantize_coeff(ideal[k]);
            quant[k] = dequantize_coeff(q[k]);
        }

        reset_dut();
        for (int k = 0; k < NUM_TAPS; k++) {
            char label[64];
            std::snprintf(label, sizeof(label), "%s-load-tap%d", name, k);
            auto e = tick(0, false, true, k, q[k]);
            check(label, dut_out(), dut->out_valid, e.first, e.second);
        }

        // FIR impulse response is finite: exactly NUM_TAPS samples, plus 1
        // cycle for the output register's latency before it starts emerging.
        const int64_t IMPULSE_AMPLITUDE = 1 << 16;
        std::vector<double> h_dut(NUM_TAPS, 0.0);
        for (int n = 0; n < NUM_TAPS; n++) {
            int64_t x = (n == 0) ? IMPULSE_AMPLITUDE : 0;
            auto e = tick(x, true, false, 0, 0);
            char label[64];
            std::snprintf(label, sizeof(label), "%s-impulse-n%d", name, n);
            check(label, dut_out(), dut->out_valid, e.first, e.second);
            h_dut[n] = double(dut_out()) / double(IMPULSE_AMPLITUDE);
        }

        // h_quant: the quantized coefficients themselves, evaluated with
        // infinite-precision arithmetic -- this is what the DUT's own
        // impulse response should match to within the single deferred
        // round+shift's rounding error.
        std::vector<double> h_quant = quant;
        // h_ideal: unquantized design, informational only (quantization
        // effect, not a correctness bar).
        std::vector<double> h_ideal = ideal;

        std::vector<double> test_freqs = { fc_norm * 0.3, fc_norm * 0.7, fc_norm * 1.5, 0.45 };
        const double TOL_DB = 0.5;
        for (double f : test_freqs) {
            double mag_dut   = dft_magnitude(h_dut, f);
            double mag_quant = dft_magnitude(h_quant, f);
            double mag_ideal = dft_magnitude(h_ideal, f);
            check_freq_response(name, f, mag_dut, mag_quant, TOL_DB);
            report_freq_response_info(name, f, mag_dut, mag_ideal);
        }
    };

    run_freq_case("lowpass-fc0.15", 0.15);
    run_freq_case("lowpass-fc0.30", 0.30);

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else              std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}