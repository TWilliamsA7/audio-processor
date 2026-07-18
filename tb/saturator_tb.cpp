#include <cstdio>
#include <cstdint>
#include <memory>
#include <verilated.h>
#include "Vsaturator_harness.h"

static constexpr int SAMPLE_WIDTH = 24;
static constexpr int64_t MAX_24 = (1LL << (SAMPLE_WIDTH - 1)) - 1;   //  8388607
static constexpr int64_t MIN_24 = -(1LL << (SAMPLE_WIDTH - 1));      // -8388608

static int errors = 0;

static int64_t sign_extend(uint64_t raw, int width) {
    int64_t v = (int64_t)raw;
    if (v & (1LL << (width - 1))) v -= (1LL << width);
    return v;
}

// Golden model: mirrors saturator's overflow check
static int64_t golden_saturate(int64_t wide_signed) {
    uint64_t raw = (uint64_t)wide_signed & ((1ULL << (SAMPLE_WIDTH + 1)) - 1);
    int sign_bit = (raw >> SAMPLE_WIDTH) & 1;
    int next_bit = (raw >> (SAMPLE_WIDTH - 1)) & 1;
    bool overflow = (sign_bit != next_bit);
    if (overflow) {
        return sign_bit ? MIN_24 : MAX_24;
    }
    return sign_extend(raw & ((1ULL << SAMPLE_WIDTH) - 1), SAMPLE_WIDTH);
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
    auto dut = std::make_unique<Vsaturator_harness>();

    dut->clk = 0;
    dut->rst_n = 0;
    dut->in_data = 0;
    dut->in_valid = 0;

    for (int i = 0; i < 6; i++) {
        dut->clk = !dut->clk;
        if (i == 2) dut->rst_n = 1;
        dut->eval();
    }
    check("post-reset", sign_extend(dut->out_data, SAMPLE_WIDTH), dut->out_valid, 0, 0);

    auto tick = [&](int64_t wide_val, int valid) {
        dut->clk = 0;
        dut->in_data = (uint32_t)((uint64_t)wide_val & ((1ULL << (SAMPLE_WIDTH + 1)) - 1));
        dut->in_valid = valid;
        dut->eval();
        dut->clk = 1;
        dut->eval();
    };

    auto out = [&]() { return sign_extend(dut->out_data, SAMPLE_WIDTH); };

    // Case 1: normal in-range positive value -- passes through unchanged.
    tick(100, 1);
    check("in-range-positive", out(), dut->out_valid, golden_saturate(100), 1);

    // Case 2: normal in-range negative value -- passes through unchanged.
    tick(-100, 1);
    check("in-range-negative", out(), dut->out_valid, golden_saturate(-100), 1);

    // Case 3: exact positive boundary (max 24-bit signed) -- no overflow.
    tick(MAX_24, 1);
    check("exact-max-boundary", out(), dut->out_valid, golden_saturate(MAX_24), 1);

    // Case 4: exact negative boundary (min 24-bit signed) -- no overflow.
    tick(MIN_24, 1);
    check("exact-min-boundary", out(), dut->out_valid, golden_saturate(MIN_24), 1);

    // Case 5: positive overflow -- one past max, must clamp to MAX_24.
    tick(MAX_24 + 1, 1);
    check("positive-overflow-clamps", out(), dut->out_valid, MAX_24, 1);

    // Case 6: larger positive overflow -- still clamps to MAX_24, not wraps.
    tick(MAX_24 + 500, 1);
    check("positive-overflow-large-clamps", out(), dut->out_valid, MAX_24, 1);

    // Case 7: negative overflow -- one past min, must clamp to MIN_24.
    tick(MIN_24 - 1, 1);
    check("negative-overflow-clamps", out(), dut->out_valid, MIN_24, 1);

    // Case 8: larger negative overflow -- still clamps to MIN_24, not wraps.
    tick(MIN_24 - 500, 1);
    check("negative-overflow-large-clamps", out(), dut->out_valid, MIN_24, 1);

    // Case 9: valid drops -- downstream.valid tracks upstream.valid with
    tick(42, 1);
    check("push-before-drain", out(), dut->out_valid, golden_saturate(42), 1);
    tick(0, 0);
    check("valid-drops-data-holds", out(), dut->out_valid, golden_saturate(42), 0);

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else              std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}