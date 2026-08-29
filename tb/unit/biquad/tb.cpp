#include <cstdio>
#include <cstdint>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>
#include <verilated.h>
#include "Vbiquad_harness.h"

// ---------------------------------------------------------------------
// Parameters (must match G_PARAMS in the Makefile / biquad_harness
// instantiation defaults).
// ---------------------------------------------------------------------
static constexpr int SAMPLE_WIDTH  = 24;
static constexpr int COEFF_WIDTH   = 16;
static constexpr int COEFF_FRAC    = 12;   // Q4.12
static constexpr int GUARD_BITS    = 1;

static constexpr int PRODUCT_WIDTH = SAMPLE_WIDTH + COEFF_WIDTH; // mult_width(24,16) = 40

// shifted_result_width(PRODUCT_WIDTH, COEFF_FRAC, GUARD_BITS) -- the function's
// actual documented use case this time: width of each x*coeff product after
// its own individual round+shift by COEFF_FRAC.
static constexpr int SHIFTED_WIDTH =
    (PRODUCT_WIDTH - COEFF_FRAC) < (GUARD_BITS + 1) ? (GUARD_BITS + 1) : (PRODUCT_WIDTH - COEFF_FRAC); // 28

// Fixed accumulator width for d1/d2 state and the internal y_full node.
// Grounded in stable-filter coefficient bounds (|a1|<2, |a2|<1) rather than
// worst-case adversarial sizing, which is circular for a recursive filter
// (see design discussion). ACC_GUARD_BITS=3 covers realistic feedback growth
// plus the internal multi-term additions; fp_pkg::saturate is the final
// backstop at the output if a pathological coefficient set ever exceeds it.
static constexpr int ACC_GUARD_BITS   = 3;
static constexpr int ACC_WIDTH        = SHIFTED_WIDTH + ACC_GUARD_BITS;          // 31
static constexpr int FB_PRODUCT_WIDTH = ACC_WIDTH + COEFF_WIDTH;                 // 47

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

// Truncates to `width` bits and sign-extends -- mirrors what a real
// `width`-bit hardware register does on overflow (wraps), which is why the
// golden model must use this rather than staying in a comfortably wide
// int64_t range unchecked.
static int64_t trunc_to_width(int64_t value, int width) {
    return sign_extend((uint64_t)value, width);
}

// Mirrors fp_pkg::round_half_up bit-for-bit (see tb/unit/fp_pkg/tb.cpp).
static int64_t golden_round_half_up(int64_t value, uint32_t frac_bits) {
    if (frac_bits == 0) return value;
    return value + (1LL << (frac_bits - 1));
}

// Mirrors fp_pkg::saturate bit-for-bit.
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
// Bit-exact golden model -- mirrors the corrected biquad.sv (per-product
// round+shift, fixed ACC_WIDTH accumulator, y_reg output fix) cycle-for-cycle.
// ---------------------------------------------------------------------

// Rounds, shifts by COEFF_FRAC, and truncates to ACC_WIDTH -- reused for both
// the feedforward (x*coeff) and feedback (y_full*coeff) products, matching
// the RTL's shared round_shift() function.
static int64_t round_shift(int64_t raw) {
    int64_t rounded = golden_round_half_up(raw, COEFF_FRAC);
    int64_t shifted = rounded >> COEFF_FRAC;   // arithmetic shift (signed)
    return trunc_to_width(shifted, ACC_WIDTH);
}

struct BiquadGolden {
    int64_t d1 = 0, d2 = 0;      // ACC_WIDTH-bit state, post-edge
    int64_t y_reg = 0;           // SAMPLE_WIDTH-bit registered output
    bool    reg_valid = false;

    int64_t b0_r = 0, b1_r = 0, b2_r = 0, a1_r = 0, a2_r = 0;   // latched coeffs

    void reset() {
        d1 = d2 = y_reg = 0;
        reg_valid = false;
        b0_r = b1_r = b2_r = a1_r = a2_r = 0;
    }

    // One clock edge. x/valid/coeff_load/b0..a2 are the values presented on
    // the ports THIS cycle (pre-edge). Returns the post-edge
    // (downstream.data, downstream.valid).
    std::pair<int64_t, bool> tick(int64_t x, bool valid, bool coeff_load,
                                   int64_t b0, int64_t b1, int64_t b2, int64_t a1, int64_t a2) {
        // --- combinational, using PRE-edge state and PRE-edge (currently
        // latched) coefficients -- a same-cycle coeff_load must NOT affect
        // this cycle's data processing. ---

        // feedforward: each x*coeff product individually rounded+shifted
        int64_t raw_b0x = x * b0_r;
        int64_t raw_b1x = x * b1_r;
        int64_t raw_b2x = x * b2_r;
        int64_t sh_b0x  = round_shift(raw_b0x);
        int64_t sh_b1x  = round_shift(raw_b1x);
        int64_t sh_b2x  = round_shift(raw_b2x);

        int64_t y_full = trunc_to_width(sh_b0x + d1, ACC_WIDTH);

        // feedback: y_full * coeff, individually rounded+shifted (wider raw
        // product than feedforward since y_full is ACC_WIDTH, not SAMPLE_WIDTH)
        int64_t raw_a1y = y_full * a1_r;
        int64_t raw_a2y = y_full * a2_r;
        int64_t sh_a1y  = round_shift(raw_a1y);
        int64_t sh_a2y  = round_shift(raw_a2y);

        int64_t d1_next = trunc_to_width(sh_b1x - sh_a1y + d2, ACC_WIDTH);
        int64_t d2_next = trunc_to_width(sh_b2x - sh_a2y, ACC_WIDTH);

        // output: rounding/shifting already done per-product, so this is
        // just a saturate -- no separate round/shift step here.
        int64_t y_sat = golden_saturate(y_full, SAMPLE_WIDTH);

        // --- registered updates, all landing on this same edge ---
        reg_valid = valid;
        if (valid) {
            d1    = d1_next;
            d2    = d2_next;
            y_reg = y_sat;
        }
        if (coeff_load) {
            b0_r = b0; b1_r = b1; b2_r = b2; a1_r = a1; a2_r = a2;
        }

        return { y_reg, reg_valid };
    }
};

// ---------------------------------------------------------------------
// Floating-point reference model + filter design math (frequency-response
// validation only -- independent of the bit-exact path above).
// ---------------------------------------------------------------------

struct FilterCoeffs { double b0, b1, b2, a1, a2; };

// RBJ-style peaking EQ, normalized frequency (cycles/sample). Coefficients
// already normalized by a0 and sign-matched to the y[n] = b0x[n]+b1x[n-1]+
// b2x[n-2] - a1y[n-1] - a2y[n-2] convention used throughout.
static FilterCoeffs design_peaking(double f0_norm, double Q, double gain_db) {
    double A     = std::pow(10.0, gain_db / 40.0);
    double w0    = 2.0 * M_PI * f0_norm;
    double alpha = std::sin(w0) / (2.0 * Q);
    double cosw0 = std::cos(w0);
    double b0 = 1 + alpha * A, b1 = -2 * cosw0, b2 = 1 - alpha * A;
    double a0 = 1 + alpha / A, a1 = -2 * cosw0, a2 = 1 - alpha / A;
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

// Test-side utility only: converts a desired float coefficient into the
// Q4.12 integer we'd drive on the b0..a2 ports. Not part of the RTL --
// the biquad's coeff_load port just latches whatever integers arrive.
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

// Double-precision DF2T, no width limits, no rounding, no saturation --
// "what the math says should happen with infinite precision."
struct BiquadReferenceF {
    double d1 = 0.0, d2 = 0.0;
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    void reset() { d1 = d2 = 0.0; }
    double tick(double x) {
        double y = b0 * x + d1;
        d1 = b1 * x - a1 * y + d2;
        d2 = b2 * x - a2 * y;
        return y;
    }
};

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
    auto dut = std::make_unique<Vbiquad_harness>();
    BiquadGolden golden;

    auto reset_dut = [&]() {
        dut->clk = 0; dut->rst_n = 0;
        dut->in_valid = 0; dut->coeff_load = 0;
        dut->b0 = dut->b1 = dut->b2 = dut->a1 = dut->a2 = 0;
        dut->in_data = 0;
        for (int i = 0; i < 6; i++) {
            dut->clk = !dut->clk;
            if (i == 2) dut->rst_n = 1;
            dut->eval();
        }
        golden.reset();
    };

    // One full clock edge: drives the DUT, advances the golden model in
    // lockstep with the same inputs, returns golden's expected output.
    auto tick = [&](int64_t x, bool valid, bool coeff_load,
                     int64_t b0, int64_t b1, int64_t b2, int64_t a1, int64_t a2) -> std::pair<int64_t, bool> {
        dut->clk = 0;
        dut->in_data    = (uint32_t)x & 0xFFFFFFu;
        dut->in_valid   = valid;
        dut->coeff_load = coeff_load;
        dut->b0 = (uint16_t)b0; dut->b1 = (uint16_t)b1; dut->b2 = (uint16_t)b2;
        dut->a1 = (uint16_t)a1; dut->a2 = (uint16_t)a2;
        dut->eval();
        dut->clk = 1;
        dut->eval();
        return golden.tick(x, valid, coeff_load, b0, b1, b2, a1, a2);
    };
    auto dut_out = [&]() { return sign_extend(dut->out_data, SAMPLE_WIDTH); };

    // ---- Directed bit-exact cases ----
    reset_dut();
    check("post-reset", dut_out(), dut->out_valid, 0, 0);

    // unity/pass-through: b0 = 1.0 in Q4.12 = 4096, everything else 0
    tick(0, false, true, 4096, 0, 0, 0, 0);
    {
        auto e = tick(1000, true, false, 4096, 0, 0, 0, 0);
        check("unity-sample1", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(-500, true, false, 4096, 0, 0, 0, 0);
        check("unity-sample2", dut_out(), dut->out_valid, e.first, e.second);
    }

    // coeff_load on the SAME cycle as a data sample -- that sample must
    // still use the OLD coefficients; new ones apply starting next cycle.
    {
        auto e = tick(777, true, true, 0, 4096, 0, 0, 0);   // switching b0:4096->0, b1:0->4096
        check("coeff-load-same-cycle-uses-old-coeffs", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(0, true, false, 0, 4096, 0, 0, 0);
        check("coeff-load-next-cycle-uses-new-coeffs", dut_out(), dut->out_valid, e.first, e.second);
    }

    // valid drop holds state (matches saturator/volume_ctrl convention)
    {
        auto e = tick(300, true, false, 0, 4096, 0, 0, 0);
        check("push-before-drain", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(0, false, false, 0, 4096, 0, 0, 0);
        check("valid-drops-holds", dut_out(), dut->out_valid, e.first, e.second);
    }

    // Coefficient saturation at the Q4.12 boundary: request a value that
    // exceeds representable range and confirm it clamps rather than wraps.
    {
        int64_t max_coeff = (1LL << (COEFF_WIDTH - 1)) - 1;    // 32767
        int64_t over_range = quantize_coeff(20.0);               // way past +8.0 ceiling
        check("coeff-quantize-clamps-to-max", over_range, 1, max_coeff, 1);
    }

    // ---- Frequency response cases ----
    auto run_freq_case = [&](const char* name, double f0_norm, double Q, double gain_db) {
        FilterCoeffs ideal = design_peaking(f0_norm, Q, gain_db);
        int64_t qb0 = quantize_coeff(ideal.b0), qb1 = quantize_coeff(ideal.b1), qb2 = quantize_coeff(ideal.b2);
        int64_t qa1 = quantize_coeff(ideal.a1), qa2 = quantize_coeff(ideal.a2);
        FilterCoeffs quant = { dequantize_coeff(qb0), dequantize_coeff(qb1), dequantize_coeff(qb2),
                                dequantize_coeff(qa1), dequantize_coeff(qa2) };

        reset_dut();
        tick(0, false, true, qb0, qb1, qb2, qa1, qa2);   // load coefficients

        const int N = 4096;
        const int64_t IMPULSE_AMPLITUDE = 1 << 16;   // modest, avoids saturation
        std::vector<double> h_dut(N);
        for (int n = 0; n < N; n++) {
            int64_t x = (n == 0) ? IMPULSE_AMPLITUDE : 0;
            auto e = tick(x, true, false, qb0, qb1, qb2, qa1, qa2);
            char label[96];
            std::snprintf(label, sizeof(label), "bit-exact-during-%s-n%d", name, n);
            check(label, dut_out(), dut->out_valid, e.first, e.second);
            h_dut[n] = double(dut_out()) / double(IMPULSE_AMPLITUDE);
        }

        BiquadReferenceF ref_quant;
        ref_quant.b0 = quant.b0; ref_quant.b1 = quant.b1; ref_quant.b2 = quant.b2;
        ref_quant.a1 = quant.a1; ref_quant.a2 = quant.a2;
        std::vector<double> h_quant(N);
        for (int n = 0; n < N; n++) h_quant[n] = ref_quant.tick(n == 0 ? 1.0 : 0.0);

        BiquadReferenceF ref_ideal;
        ref_ideal.b0 = ideal.b0; ref_ideal.b1 = ideal.b1; ref_ideal.b2 = ideal.b2;
        ref_ideal.a1 = ideal.a1; ref_ideal.a2 = ideal.a2;
        std::vector<double> h_ideal(N);
        for (int n = 0; n < N; n++) h_ideal[n] = ref_ideal.tick(n == 0 ? 1.0 : 0.0);

        // on-peak, half/double the center freq (checks bandwidth/shape, not
        // just peak height), and a far-away reference point (checks the
        // filter returns to near-unity away from the peak).
        std::vector<double> test_freqs = { f0_norm, f0_norm * 0.5, f0_norm * 2.0, 0.25 };
        const double TOL_DB = 0.5;
        for (double f : test_freqs) {
            double mag_dut   = dft_magnitude(h_dut, f);
            double mag_quant = dft_magnitude(h_quant, f);
            double mag_ideal = dft_magnitude(h_ideal, f);
            check_freq_response(name, f, mag_dut, mag_quant, TOL_DB);
            report_freq_response_info(name, f, mag_dut, mag_ideal);
        }
    };

    run_freq_case("mild-peaking",   0.02, 0.707, 6.0);
    run_freq_case("high-q-peaking", 0.02, 8.0,  12.0);

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else              std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}