#include <cstdio>
#include <cstdint>
#include <memory>
#include <verilated.h>
#include "Vvolume_ctrl_harness.h"

static constexpr int SAMPLE_WIDTH = 24;
static constexpr int FRACTIONAL_BITS = 6;

static int errors = 0;

static int64_t golden_gain(int32_t data, uint32_t gain) {
    int64_t product = (int64_t)data * (int64_t)gain;               // signed x unsigned(zero-ext)
    int64_t rounded = product + (1LL << (FRACTIONAL_BITS - 1));
    int64_t shifted = rounded >> FRACTIONAL_BITS;                   // arithmetic shift

    const int64_t out_width = SAMPLE_WIDTH + 1;                     // guard bit
    const int64_t mask = (1LL << out_width) - 1;
    int64_t truncated = shifted & mask;
    if (truncated & (1LL << SAMPLE_WIDTH)) truncated -= (1LL << out_width);  // sign-extend
    return truncated;
}

static int32_t sign_extend(uint32_t raw, int width) {
    int64_t v = (int64_t)raw;
    if (v & (1LL << (width - 1))) v -= (1LL << width);
    return (int32_t)v;
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

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto dut = std::make_unique<Vvolume_ctrl_harness>();

    dut->clk = 0;
    dut->rst_n = 0;
    dut->in_data = 0;
    dut->in_valid = 0;
    dut->gain = 0;

    for (int i = 0; i < 6; i++) {
        dut->clk = !dut->clk;
        if (i == 2) dut->rst_n = 1;
        dut->eval();
    }
    check("post-reset", sign_extend(dut->out_data, SAMPLE_WIDTH + 1), dut->out_valid, 0, 0);


    auto tick = [&](int data, uint32_t gain, int valid) {
        dut->clk = 0;
        dut->in_data = (uint32_t)data & 0xFFFFFFu; 
        dut->gain = gain;
        dut->in_valid = valid;
        dut->eval();
        dut->clk = 1;
        dut->eval();
    };

    auto out = [&]() { return sign_extend(dut->out_data, SAMPLE_WIDTH + 1); };

    // Case 1: unity gain (0x40 in Q2.6), positive input.
    tick(10, 0x40, 1);
    check("unity-gain-pos", out(), dut->out_valid, golden_gain(10, 0x40), 1);

    // Case 2: unity gain, negative input.
    tick(-20, 0x40, 1);
    check("unity-gain-neg", out(), dut->out_valid, golden_gain(-20, 0x40), 1);

    // Case 3: zero gain (mute).
    tick(50, 0x00, 1);
    check("zero-gain-mute", out(), dut->out_valid, golden_gain(50, 0x00), 1);

    // Case 4: half gain (0x20 in Q2.6 = 0.5), exercising round-half-up.
    tick(1, 0x20, 1);
    check("half-gain-rounds", out(), dut->out_valid, golden_gain(1, 0x20), 1);

    // Case 5: gain > unity (boost), positive input -- exercises the guard
    tick(100, 0x80, 1);  // 0x80 = 2.0x in Q2.6
    check("boost-gain-uses-guard-bit", out(), dut->out_valid, golden_gain(100, 0x80), 1);

    // Case 6: valid drops to 0
    tick(5, 0x40, 1);
    check("push-before-drain", out(), dut->out_valid, golden_gain(5, 0x40), 1);
    tick(0, 0x40, 0);
    check("valid-drops-data-holds", out(), dut->out_valid, golden_gain(5, 0x40), 0);

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else              std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}