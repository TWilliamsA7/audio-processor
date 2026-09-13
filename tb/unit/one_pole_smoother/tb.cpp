#include <cstdio>
#include <cstdint>
#include <cmath>
#include <memory>
#include <utility>
#include <verilated.h>
#include "Vone_pole_smoother_harness.h"

// ---------------------------------------------------------------------
// Parameters (must match G_PARAMS / harness defaults).
// ---------------------------------------------------------------------
static constexpr int WIDTH          = 24;
static constexpr int COEFF_WIDTH    = 16;
static constexpr int COEFF_FRAC     = 16;   // Q0.16
static constexpr int ACC_GUARD_BITS = 2;

static constexpr int ACC_WIDTH     = WIDTH + ACC_GUARD_BITS;          // 26
static constexpr int DIFF_WIDTH    = ACC_WIDTH + 1;                   // 27
static constexpr int PRODUCT_WIDTH = DIFF_WIDTH + COEFF_WIDTH;        // 43

static int errors = 0;

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
// Bit-exact golden model -- mirrors one_pole_smoother.sv cycle-for-cycle.
// target/state_out are unsigned; coeff is unsigned Q0.COEFF_FRAC.
// ---------------------------------------------------------------------

static int64_t round_shift(int64_t raw) {
    int64_t rounded = golden_round_half_up(raw, COEFF_FRAC);
    int64_t shifted = rounded >> COEFF_FRAC;
    return trunc_to_width(shifted, ACC_WIDTH);
}

struct SmootherGolden {
    int64_t state_full = 0;
    int64_t state_out  = 0;
    bool    valid_out  = false;

    void reset() { state_full = 0; state_out = 0; valid_out = false; }

    std::pair<int64_t, bool> tick(uint32_t target, uint32_t coeff, bool valid) {
        int64_t diff = trunc_to_width((int64_t)target - state_full, DIFF_WIDTH);
        int64_t raw_product = diff * (int64_t)coeff;
        int64_t sh_product  = round_shift(raw_product);
        int64_t state_next  = trunc_to_width(state_full + sh_product, ACC_WIDTH);
        int64_t out_next    = (state_next < 0) ? 0 : (state_next & ((1LL << WIDTH) - 1));

        valid_out = valid;
        if (valid) {
            state_full = state_next;
            state_out  = out_next;
        }
        return { state_out, valid_out };
    }
};

static int64_t quantize_coeff_q016(double value) {
    int64_t max_val = (1LL << COEFF_WIDTH) - 1;
    int64_t rounded = (int64_t)std::llround(value * double(1LL << COEFF_FRAC));
    if (rounded > max_val) rounded = max_val;
    if (rounded < 0) rounded = 0;
    return rounded;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto dut = std::make_unique<Vone_pole_smoother_harness>();
    SmootherGolden golden;

    auto reset_dut = [&]() {
        dut->clk = 0; dut->rst_n = 0;
        dut->target = 0; dut->coeff = 0; dut->valid_in = 0;
        for (int i = 0; i < 6; i++) {
            dut->clk = !dut->clk;
            if (i == 2) dut->rst_n = 1;
            dut->eval();
        }
        golden.reset();
    };

    auto tick = [&](uint32_t target, uint32_t coeff, bool valid) -> std::pair<int64_t, bool> {
        dut->clk = 0;
        dut->target   = target;
        dut->coeff    = coeff;
        dut->valid_in = valid;
        dut->eval();
        dut->clk = 1;
        dut->eval();
        return golden.tick(target, coeff, valid);
    };
    auto dut_out = [&]() { return (int64_t)(dut->state_out & ((1u << WIDTH) - 1)); };

    // ---- Directed bit-exact cases ----
    reset_dut();
    check("post-reset", dut_out(), dut->valid_out, 0, 0);

    // Step to full-scale target with coeff = 0.5 -- climbs geometrically.
    const uint32_t TARGET_FULL = 1u << 20;
    const uint32_t COEFF_HALF  = (uint32_t)quantize_coeff_q016(0.5);
    for (int n = 0; n < 10; n++) {
        char label[32];
        std::snprintf(label, sizeof(label), "climb-n%d", n);
        auto e = tick(TARGET_FULL, COEFF_HALF, true);
        check(label, dut_out(), dut->valid_out, e.first, e.second);
    }

    // Step target back to zero -- decays.
    for (int n = 0; n < 10; n++) {
        char label[32];
        std::snprintf(label, sizeof(label), "decay-n%d", n);
        auto e = tick(0, COEFF_HALF, true);
        check(label, dut_out(), dut->valid_out, e.first, e.second);
    }

    // coeff change on the SAME cycle as a data sample -- old coeff applies.
    {
        auto e = tick(TARGET_FULL, (uint32_t)quantize_coeff_q016(0.9), true);
        check("coeff-change-same-cycle-uses-committed-value", dut_out(), dut->valid_out, e.first, e.second);
    }

    // valid drop holds state.
    {
        auto e1 = tick(TARGET_FULL, COEFF_HALF, true);
        check("push-before-drain", dut_out(), dut->valid_out, e1.first, e1.second);
        auto e2 = tick(0, COEFF_HALF, false);
        check("valid-drops-holds", dut_out(), dut->valid_out, e2.first, e2.second);
    }

    // ---- Analytical: time-constant check, same style as envelope_follower ----
    auto run_time_constant_case = [&](const char* name, double tau_samples) {
        double coeff_ideal = 1.0 - std::exp(-1.0 / tau_samples);
        uint32_t q = (uint32_t)quantize_coeff_q016(coeff_ideal);

        reset_dut();
        const uint32_t TARGET = 1u << 20;
        const int N = (int)(tau_samples * 6);
        double y_dut_at_tau = -1.0;

        for (int n = 0; n < N; n++) {
            auto e = tick(TARGET, q, true);
            char label[64];
            std::snprintf(label, sizeof(label), "bit-exact-during-%s-n%d", name, n);
            check(label, dut_out(), dut->valid_out, e.first, e.second);
            if (n == (int)std::llround(tau_samples) - 1) y_dut_at_tau = double(dut_out());
        }

        double expected_frac = 1.0 - std::exp(-1.0);
        double dut_frac = y_dut_at_tau / double(TARGET);
        const double TOL = 0.05;
        if (std::abs(dut_frac - expected_frac) > TOL) {
            std::printf("FAIL [%s-time-constant]: got %.4f of target, expected ~%.4f\n",
                         name, dut_frac, expected_frac);
            errors++;
        } else {
            std::printf("PASS [%s-time-constant]: got %.4f of target (expected ~%.4f)\n",
                         name, dut_frac, expected_frac);
        }
    };

    run_time_constant_case("tau10", 10.0);
    run_time_constant_case("tau50", 50.0);

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else              std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}