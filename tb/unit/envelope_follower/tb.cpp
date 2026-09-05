// tb/unit/envelope_follower/tb.cpp
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>
#include <verilated.h>
#include "Venvelope_follower_harness.h"

// ---------------------------------------------------------------------
// Parameters (must match G_PARAMS / harness defaults).
// ---------------------------------------------------------------------
static constexpr int SAMPLE_WIDTH  = 24;
static constexpr int COEFF_WIDTH   = 16;
static constexpr int COEFF_FRAC    = 16;   // Q0.16, coeff in [0,1)

static constexpr int ACC_GUARD_BITS = 2;
static constexpr int ACC_WIDTH      = SAMPLE_WIDTH + ACC_GUARD_BITS;      // 26
static constexpr int ABS_WIDTH      = SAMPLE_WIDTH;                       // 24
static constexpr int NEG_WIDTH      = SAMPLE_WIDTH + 1;                   // 25
static constexpr int DIFF_WIDTH     = ACC_WIDTH + 1;                      // 27
static constexpr int PRODUCT_WIDTH  = DIFF_WIDTH + COEFF_WIDTH;           // 43 (mult_width)

static constexpr int ADDR_ATTACK  = 0;
static constexpr int ADDR_RELEASE = 1;

static int errors = 0;

// ---------------------------------------------------------------------
// Bit-level helpers (same idioms as biquad/eq tb.cpp)
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

static int64_t round_shift(int64_t raw) {
    int64_t rounded = golden_round_half_up(raw, COEFF_FRAC);
    int64_t shifted = rounded >> COEFF_FRAC;   // arithmetic shift
    return trunc_to_width(shifted, ACC_WIDTH);
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
// Bit-exact golden model -- mirrors envelope_follower.sv cycle-for-cycle,
// including the abs(MIN_INT) safe-negation path and the floor-at-zero
// output convention (no upper saturate -- relies on convexity bound).
// ---------------------------------------------------------------------

struct EnvGolden {
    int64_t env_full = 0;      // ACC_WIDTH-bit signed feedback state
    int64_t env_reg  = 0;      // SAMPLE_WIDTH-bit unsigned output register
    bool    reg_valid = false;
    uint32_t coeff_mem[2] = {0, 0};

    void reset() {
        env_full = 0; env_reg = 0; reg_valid = false;
        coeff_mem[0] = coeff_mem[1] = 0;
    }

    // x is the raw SAMPLE_WIDTH-bit input (as a signed value already).
    std::pair<int64_t, bool> tick(int64_t x_signed, bool valid, bool we, int addr, uint32_t data) {
        // --- combinational, using PRE-edge coeff_mem and PRE-edge env_full ---
        int64_t x_negated = -(int64_t)x_signed;             // safe: 64-bit headroom
        uint32_t abs_x = (x_signed < 0)
                             ? (uint32_t)(x_negated & 0xFFFFFF)   // ABS_WIDTH=24 mask
                             : (uint32_t)(x_signed  & 0xFFFFFF);

        int64_t diff = trunc_to_width((int64_t)abs_x - env_full, DIFF_WIDTH);
        bool rising = diff > 0;
        uint32_t coeff_sel = rising ? coeff_mem[ADDR_ATTACK] : coeff_mem[ADDR_RELEASE];

        int64_t raw_product = diff * (int64_t)coeff_sel;
        int64_t sh_product  = round_shift(raw_product);

        int64_t env_next = trunc_to_width(env_full + sh_product, ACC_WIDTH);

        int64_t env_out = (env_next < 0) ? 0 : (env_next & 0xFFFFFF);  // low SAMPLE_WIDTH bits

        // --- registered updates ---
        reg_valid = valid;
        if (valid) {
            env_full = env_next;
            env_reg  = env_out;
        }
        if (we) {
            coeff_mem[addr] = data;
        }

        return { env_reg, reg_valid };
    }
};

static int64_t quantize_coeff_q016(double value) {
    int64_t max_val = (1LL << COEFF_WIDTH) - 1;   // unsigned, all-fractional
    int64_t rounded = (int64_t)std::llround(value * double(1LL << COEFF_FRAC));
    if (rounded > max_val) rounded = max_val;
    if (rounded < 0) rounded = 0;
    return rounded;
}
static double dequantize_coeff_q016(int64_t raw) {
    return double(raw) / double(1LL << COEFF_FRAC);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto dut = std::make_unique<Venvelope_follower_harness>();
    EnvGolden golden;

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

    auto tick = [&](int64_t x_signed, bool valid, bool we, int addr, uint32_t data) -> std::pair<int64_t, bool> {
        dut->clk = 0;
        dut->in_data    = (uint32_t)x_signed & 0xFFFFFFu;
        dut->in_valid   = valid;
        dut->coeff_we   = we;
        dut->coeff_addr = (uint32_t)addr;
        dut->coeff_data = data;
        dut->eval();
        dut->clk = 1;
        dut->eval();
        return golden.tick(x_signed, valid, we, addr, data);
    };
    // downstream.data is UNSIGNED -- do not sign-extend when reading it back.
    auto dut_out = [&]() { return (int64_t)(dut->out_data & 0xFFFFFFu); };

    // ---- Directed bit-exact cases ----
    reset_dut();
    check("post-reset", dut_out(), dut->out_valid, 0, 0);

    // Load attack=0.5 (32768), release=0.01 (~655) in Q0.16.
    int64_t attack_coeff  = quantize_coeff_q016(0.5);
    int64_t release_coeff = quantize_coeff_q016(0.01);
    {
        auto e = tick(0, false, true, ADDR_ATTACK, (uint32_t)attack_coeff);
        check("load-attack", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(0, false, true, ADDR_RELEASE, (uint32_t)release_coeff);
        check("load-release", dut_out(), dut->out_valid, e.first, e.second);
    }

    // coeff_we on the SAME cycle as a data sample -- must use OLD coeff.
    {
        // switch attack 0.5 -> 0.9, same cycle as a rising sample
        auto e = tick(1000000, true, true, ADDR_ATTACK, (uint32_t)quantize_coeff_q016(0.9));
        check("coeff-we-same-cycle-uses-old-coeff", dut_out(), dut->out_valid, e.first, e.second);
    }
    {
        auto e = tick(1000000, true, false, 0, 0);
        check("coeff-we-next-cycle-uses-new-coeff", dut_out(), dut->out_valid, e.first, e.second);
    }

    // abs(MIN_INT) edge case: input = -2^23, must not overflow/wrap.
    reset_dut();
    tick(0, false, true, ADDR_ATTACK, (uint32_t)attack_coeff);
    tick(0, false, true, ADDR_RELEASE, (uint32_t)release_coeff);
    {
        int64_t MIN_24 = -(1LL << (SAMPLE_WIDTH - 1));   // -8388608
        auto e = tick(MIN_24, true, false, 0, 0);
        check("abs-min-int-edge-case", dut_out(), dut->out_valid, e.first, e.second);
    }

    // valid drop holds state (bubble)
    {
        auto e1 = tick(500000, true, false, 0, 0);
        check("push-before-drain", dut_out(), dut->out_valid, e1.first, e1.second);
        auto e2 = tick(0, false, false, 0, 0);
        check("valid-drops-holds", dut_out(), dut->out_valid, e2.first, e2.second);
    }

    // Rising then falling: step to a positive amplitude, let it climb under
    // attack for several samples, then drop input to 0 and confirm it
    // switches to (much slower) release rather than continuing to use attack.
    reset_dut();
    tick(0, false, true, ADDR_ATTACK, (uint32_t)attack_coeff);
    tick(0, false, true, ADDR_RELEASE, (uint32_t)release_coeff);
    {
        const int64_t STEP = 1 << 22;   // half-scale step
        for (int n = 0; n < 20; n++) {
            char label[32];
            std::snprintf(label, sizeof(label), "attack-climb-n%d", n);
            auto e = tick(STEP, true, false, 0, 0);
            check(label, dut_out(), dut->out_valid, e.first, e.second);
        }
        for (int n = 0; n < 50; n++) {
            char label[32];
            std::snprintf(label, sizeof(label), "release-decay-n%d", n);
            auto e = tick(0, true, false, 0, 0);
            check(label, dut_out(), dut->out_valid, e.first, e.second);
        }
    }

    // ---- Floating-point / analytical validation: does the coefficient
    // actually produce the time constant it should? Bit-exactness against
    // our own golden model doesn't catch a conceptually wrong recursion --
    // this compares against the closed-form step response of a first-order
    // IIR: y[n] = target * (1 - (1-coeff)^n), same "second layer" role as
    // biquad/eq's frequency-response check.
    auto run_time_constant_case = [&](const char* name, double tau_samples) {
        double coeff_ideal = 1.0 - std::exp(-1.0 / tau_samples);
        int64_t q = quantize_coeff_q016(coeff_ideal);
        double coeff_quant = dequantize_coeff_q016(q);

        reset_dut();
        tick(0, false, true, ADDR_ATTACK, (uint32_t)q);
        tick(0, false, true, ADDR_RELEASE, (uint32_t)q);   // symmetric for this check

        const int64_t TARGET = 1 << 20;
        const int N = (int)(tau_samples * 6);   // run out to ~6 time constants
        double y_dut_at_tau = -1.0, y_quant_at_tau = -1.0;
        double y_ref = 0.0;   // exact double-precision recursion, quantized coeff

        for (int n = 0; n < N; n++) {
            auto e = tick(TARGET, true, false, 0, 0);
            char label[64];
            std::snprintf(label, sizeof(label), "bit-exact-during-%s-n%d", name, n);
            check(label, dut_out(), dut->out_valid, e.first, e.second);

            y_ref = y_ref + coeff_quant * (double(TARGET) - y_ref);

            if (n == (int)std::llround(tau_samples) - 1) {
                y_dut_at_tau   = double(dut_out());
                y_quant_at_tau = y_ref;
            }
        }

        double expected_frac = 1.0 - std::exp(-1.0);   // ~0.6321 at n=tau
        double dut_frac   = y_dut_at_tau / double(TARGET);
        double quant_frac = y_quant_at_tau / double(TARGET);

        const double TOL = 0.05;   // 5%, covers quantization + rounding
        if (std::abs(dut_frac - expected_frac) > TOL) {
            std::printf("FAIL [%s-time-constant]: at n=tau, got %.4f of target, expected ~%.4f (analytical)\n",
                         name, dut_frac, expected_frac);
            errors++;
        } else {
            std::printf("PASS [%s-time-constant]: at n=tau, got %.4f of target (analytical ~%.4f, quantized-coeff ref %.4f)\n",
                         name, dut_frac, expected_frac, quant_frac);
        }
    };

    run_time_constant_case("tau10",  10.0);
    run_time_constant_case("tau50",  50.0);
    run_time_constant_case("tau200", 200.0);

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else              std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}