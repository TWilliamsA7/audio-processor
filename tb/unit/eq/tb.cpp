#include <cstdio>
#include <cstdint>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>
#include <verilated.h>
#include "Veq_harness.h"

// ---------------------------------------------------------------------
// Parameters (must match G_PARAMS in the Makefile / eq_harness defaults).
// Mirrors the width derivation in biquad's tb.cpp -- eq.sv adds no new
// arithmetic of its own, so these are exactly biquad's constants, plus
// NUM_BANDS for the cascade.
// ---------------------------------------------------------------------
static constexpr int SAMPLE_WIDTH  = 24;
static constexpr int COEFF_WIDTH   = 16;
static constexpr int COEFF_FRAC    = 12;   // Q4.12
static constexpr int GUARD_BITS    = 1;
static constexpr int NUM_BANDS     = 5;

static constexpr int PRODUCT_WIDTH = SAMPLE_WIDTH + COEFF_WIDTH; // mult_width(24,16) = 40
static constexpr int SHIFTED_WIDTH =
    (PRODUCT_WIDTH - COEFF_FRAC) < (GUARD_BITS + 1) ? (GUARD_BITS + 1) : (PRODUCT_WIDTH - COEFF_FRAC); // 28
static constexpr int ACC_GUARD_BITS   = 3;
static constexpr int ACC_WIDTH        = SHIFTED_WIDTH + ACC_GUARD_BITS;          // 31
static constexpr int FB_PRODUCT_WIDTH = ACC_WIDTH + COEFF_WIDTH;                 // 47

static int errors = 0;

// ---------------------------------------------------------------------
// Bit-level helpers (identical to biquad's tb.cpp)
// ---------------------------------------------------------------------

static int64_t sign_extend(uint64_t raw, int width) {
    uint64_t mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
    int64_t v = (int64_t)(raw & mask);
    if (v & (1LL << (width - 1))) v -= (1LL << width);
    return v;
}

static int64_t trunc_to_width(int64_t value, int width) {
    return sign_extend((uint64_t)value, width);
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
// Bit-exact golden model: one BiquadGolden per band (identical math to
// biquad's own tb.cpp), chained by EqGolden which reproduces eq.sv's
// coeff_load/band_sel demux exactly. If routing, latency, or per-band
// coefficient isolation is wrong in the RTL, this diverges from the DUT.
// ---------------------------------------------------------------------

static int64_t round_shift(int64_t raw) {
    int64_t rounded = golden_round_half_up(raw, COEFF_FRAC);
    int64_t shifted = rounded >> COEFF_FRAC;
    return trunc_to_width(shifted, ACC_WIDTH);
}

struct BiquadGolden {
    int64_t d1 = 0, d2 = 0;
    int64_t y_reg = 0;
    bool    reg_valid = false;
    int64_t b0_r = 0, b1_r = 0, b2_r = 0, a1_r = 0, a2_r = 0;

    void reset() {
        d1 = d2 = y_reg = 0;
        reg_valid = false;
        b0_r = b1_r = b2_r = a1_r = a2_r = 0;
    }

    std::pair<int64_t, bool> tick(int64_t x, bool valid, bool coeff_load,
                                   int64_t b0, int64_t b1, int64_t b2, int64_t a1, int64_t a2) {
        int64_t raw_b0x = x * b0_r;
        int64_t raw_b1x = x * b1_r;
        int64_t raw_b2x = x * b2_r;
        int64_t sh_b0x  = round_shift(raw_b0x);
        int64_t sh_b1x  = round_shift(raw_b1x);
        int64_t sh_b2x  = round_shift(raw_b2x);

        int64_t y_full = trunc_to_width(sh_b0x + d1, ACC_WIDTH);

        int64_t raw_a1y = y_full * a1_r;
        int64_t raw_a2y = y_full * a2_r;
        int64_t sh_a1y  = round_shift(raw_a1y);
        int64_t sh_a2y  = round_shift(raw_a2y);

        int64_t d1_next = trunc_to_width(sh_b1x - sh_a1y + d2, ACC_WIDTH);
        int64_t d2_next = trunc_to_width(sh_b2x - sh_a2y, ACC_WIDTH);

        int64_t y_sat = golden_saturate(y_full, SAMPLE_WIDTH);

        reg_valid = valid;
        if (valid) {
            d1 = d1_next;
            d2 = d2_next;
            y_reg = y_sat;
        }
        if (coeff_load) {
            b0_r = b0; b1_r = b1; b2_r = b2; a1_r = a1; a2_r = a2;
        }

        return { y_reg, reg_valid };
    }
};

// Cascade of NUM_BANDS BiquadGolden, reproducing eq.sv's band_sel demux AND
// its inter-stage registering: link[i] is a flip-flop output (biquad's
// y_reg), so band i+1 samples band i's output from the PREVIOUS edge, not
// the value band i computes this same edge. Chaining combinationally within
// one tick() (as an early version of this model did) collapses the whole
// cascade to biquad's own 1-cycle latency instead of NUM_BANDS cycles --
// exactly the kind of inter-module latency assumption the roadmap's "no
// module in isolation" principle exists to catch.
struct EqGolden {
    BiquadGolden bands[NUM_BANDS];

    void reset() {
        for (auto& b : bands) b.reset();
    }

    std::pair<int64_t, bool> tick(int64_t x, bool valid, bool coeff_load, int band_sel,
                                   int64_t b0, int64_t b1, int64_t b2, int64_t a1, int64_t a2) {
        // Snapshot every band's registered output from BEFORE this edge --
        // that's what's actually present on each inter-stage wire during
        // this cycle.
        int64_t old_y[NUM_BANDS];
        bool old_valid[NUM_BANDS];
        for (int i = 0; i < NUM_BANDS; i++) {
            old_y[i] = bands[i].y_reg;
            old_valid[i] = bands[i].reg_valid;
        }

        for (int i = 0; i < NUM_BANDS; i++) {
            int64_t in_x   = (i == 0) ? x     : old_y[i - 1];
            bool    in_val = (i == 0) ? valid : old_valid[i - 1];
            bool this_load = coeff_load && (band_sel == i);
            bands[i].tick(in_x, in_val, this_load, b0, b1, b2, a1, a2);
        }

        return { bands[NUM_BANDS - 1].y_reg, bands[NUM_BANDS - 1].reg_valid };
    }
};

// ---------------------------------------------------------------------
// Floating-point reference model + filter design math (frequency-response
// validation only). Identical design_peaking / quantize / DFT machinery to
// biquad's tb.cpp, generalized to a cascade of NUM_BANDS filters.
// ---------------------------------------------------------------------

struct FilterCoeffs { double b0, b1, b2, a1, a2; };

static FilterCoeffs design_peaking(double f0_norm, double Q, double gain_db) {
    double A     = std::pow(10.0, gain_db / 40.0);
    double w0    = 2.0 * M_PI * f0_norm;
    double alpha = std::sin(w0) / (2.0 * Q);
    double cosw0 = std::cos(w0);
    double b0 = 1 + alpha * A, b1 = -2 * cosw0, b2 = 1 - alpha * A;
    double a0 = 1 + alpha / A, a1 = -2 * cosw0, a2 = 1 - alpha / A;
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
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

// Cascade of NUM_BANDS floating reference biquads.
struct EqReferenceF {
    BiquadReferenceF bands[NUM_BANDS];
    double tick(double x) {
        double v = x;
        for (auto& b : bands) v = b.tick(v);
        return v;
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

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto dut = std::make_unique<Veq_harness>();
    EqGolden golden;

    auto reset_dut = [&]() {
        dut->clk = 0; dut->rst_n = 0;
        dut->in_valid = 0; dut->coeff_load = 0; dut->band_sel = 0;
        dut->b0 = dut->b1 = dut->b2 = dut->a1 = dut->a2 = 0;
        dut->in_data = 0;
        for (int i = 0; i < 6; i++) {
            dut->clk = !dut->clk;
            if (i == 2) dut->rst_n = 1;
            dut->eval();
        }
        golden.reset();
    };

    auto tick = [&](int64_t x, bool valid, bool coeff_load, int band_sel,
                     int64_t b0, int64_t b1, int64_t b2, int64_t a1, int64_t a2) -> std::pair<int64_t, bool> {
        dut->clk = 0;
        dut->in_data    = (uint32_t)x & 0xFFFFFFu;
        dut->in_valid   = valid;
        dut->coeff_load = coeff_load;
        dut->band_sel   = (uint32_t)band_sel;
        dut->b0 = (uint16_t)b0; dut->b1 = (uint16_t)b1; dut->b2 = (uint16_t)b2;
        dut->a1 = (uint16_t)a1; dut->a2 = (uint16_t)a2;
        dut->eval();
        dut->clk = 1;
        dut->eval();
        return golden.tick(x, valid, coeff_load, band_sel, b0, b1, b2, a1, a2);
    };
    auto dut_out = [&]() { return sign_extend(dut->out_data, SAMPLE_WIDTH); };

    // ---- Directed bit-exact cases ----
    reset_dut();
    check("post-reset", dut_out(), dut->out_valid, 0, 0);

    // Load each band to unity passthrough (b0=4096, rest 0) one at a time,
    // via band_sel -- this exercises the coeff_load demux itself: if
    // routing were wrong (e.g. broadcasting to all bands, or off-by-one on
    // band_sel), the bit-exact check against the golden cascade -- which
    // encodes the *intended* per-band routing -- will diverge.
    for (int band = 0; band < NUM_BANDS; band++) {
        char label[64];
        std::snprintf(label, sizeof(label), "load-unity-band%d", band);
        auto e = tick(0, false, true, band, 4096, 0, 0, 0, 0);
        check(label, dut_out(), dut->out_valid, e.first, e.second);
    }

    // Flush the pipeline (NUM_BANDS cycles) with valid=0, then push samples
    // through the now-all-unity cascade and confirm passthrough re-emerges
    // exactly TOTAL_LATENCY (=NUM_BANDS) cycles later. Latency isn't
    // asserted separately -- it falls out of the bit-exact check itself,
    // since the golden cascade only reports valid after the same number of
    // ticks the RTL pipeline needs.
    for (int i = 0; i < NUM_BANDS; i++) {
        auto e = tick(0, false, false, 0, 0, 0, 0, 0, 0);
        check("flush", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(1000, true, false, 0, 0, 0, 0, 0, 0);
        check("unity-cascade-sample1", dut_out(), dut->out_valid, e.first, e.second);
    }
    for (int i = 0; i < NUM_BANDS - 1; i++) {
        char label[32];
        std::snprintf(label, sizeof(label), "unity-cascade-drain%d", i);
        auto e = tick(0, false, false, 0, 0, 0, 0, 0, 0);
        check(label, dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        // The impulse should surface here, exactly NUM_BANDS cycles after
        // it entered -- confirmed against golden, not a hardcoded index.
        auto e = tick(0, false, false, 0, 0, 0, 0, 0, 0);
        check("unity-cascade-impulse-emerges", dut_out(), dut->out_valid, e.first, e.second);
        if (e.second && e.first != 1000) {
            std::printf("FAIL [unity-cascade-impulse-value]: expected 1000, got %lld\n", (long long)e.first);
            errors++;
        } else if (e.second) {
            std::printf("PASS [unity-cascade-impulse-value]: %lld\n", (long long)e.first);
        }
    }

    // Reload band 2 mid-stream (b0=0, rest 0 -> mutes everything downstream
    // of band 2) and confirm bands 0/1/3/4 are untouched -- i.e. this
    // doesn't corrupt neighboring bands' latched coefficients.
    {
        auto e = tick(500, true, true, 2, 0, 0, 0, 0, 0);
        check("reload-band2-mute-same-cycle-uses-old-coeffs", dut_out(), dut->out_valid, e.first, e.second);
    }
    for (int i = 0; i < NUM_BANDS; i++) {
        char label[32];
        std::snprintf(label, sizeof(label), "post-reload-drain%d", i);
        auto e = tick(0, false, false, 0, 0, 0, 0, 0, 0);
        check(label, dut_out(), dut->out_valid, e.first, e.second);
    }

    // ---- Frequency response: realistic 5-band parametric EQ ----
    // Roughly low / low-mid / mid / high-mid / high, boosting some and
    // cutting others -- representative of an actual mixing EQ move rather
    // than a single isolated band.
    struct BandSpec { double f0_norm, Q, gain_db; };
    static const BandSpec spec[NUM_BANDS] = {
        { 0.01, 0.71,  4.0 },   // low
        { 0.03, 1.00, -3.0 },   // low-mid cut
        { 0.08, 1.41,  5.0 },   // mid boost
        { 0.15, 1.00, -4.0 },   // high-mid cut
        { 0.30, 0.71,  3.0 },   // high
    };

    reset_dut();
    FilterCoeffs ideal[NUM_BANDS];
    FilterCoeffs quant[NUM_BANDS];
    int64_t q[NUM_BANDS][5];
    for (int band = 0; band < NUM_BANDS; band++) {
        ideal[band] = design_peaking(spec[band].f0_norm, spec[band].Q, spec[band].gain_db);
        q[band][0] = quantize_coeff(ideal[band].b0);
        q[band][1] = quantize_coeff(ideal[band].b1);
        q[band][2] = quantize_coeff(ideal[band].b2);
        q[band][3] = quantize_coeff(ideal[band].a1);
        q[band][4] = quantize_coeff(ideal[band].a2);
        quant[band] = { dequantize_coeff(q[band][0]), dequantize_coeff(q[band][1]),
                         dequantize_coeff(q[band][2]), dequantize_coeff(q[band][3]),
                         dequantize_coeff(q[band][4]) };
        tick(0, false, true, band, q[band][0], q[band][1], q[band][2], q[band][3], q[band][4]);
    }

    const int N = 4096;
    const int64_t IMPULSE_AMPLITUDE = 1 << 16;
    std::vector<double> h_dut(N);
    for (int n = 0; n < N; n++) {
        int64_t x = (n == 0) ? IMPULSE_AMPLITUDE : 0;
        auto e = tick(x, true, false, 0, 0, 0, 0, 0, 0);
        char label[64];
        std::snprintf(label, sizeof(label), "bit-exact-during-freq-sweep-n%d", n);
        check(label, dut_out(), dut->out_valid, e.first, e.second);
        h_dut[n] = double(dut_out()) / double(IMPULSE_AMPLITUDE);
    }

    EqReferenceF ref_quant;
    for (int band = 0; band < NUM_BANDS; band++) {
        ref_quant.bands[band].b0 = quant[band].b0; ref_quant.bands[band].b1 = quant[band].b1;
        ref_quant.bands[band].b2 = quant[band].b2; ref_quant.bands[band].a1 = quant[band].a1;
        ref_quant.bands[band].a2 = quant[band].a2;
    }
    std::vector<double> h_quant(N);
    for (int n = 0; n < N; n++) h_quant[n] = ref_quant.tick(n == 0 ? 1.0 : 0.0);

    std::vector<double> test_freqs = { 0.01, 0.03, 0.08, 0.15, 0.30, 0.45 };
    const double TOL_DB = 0.5;
    for (double f : test_freqs) {
        double mag_dut   = dft_magnitude(h_dut, f);
        double mag_quant = dft_magnitude(h_quant, f);
        check_freq_response("5-band-eq", f, mag_dut, mag_quant, TOL_DB);
    }

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}