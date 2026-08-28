#include <cstdio>
#include <cstdint>
#include <memory>
#include <verilated.h>
#include "Vfp_pkg_harness.h"

static int errors = 0;

// ---- Golden models, mirroring fp_pkg.sv bit-for-bit ----

static int64_t golden_round_half_up(int64_t value, uint32_t frac_bits) {
    if (frac_bits == 0) return value;
    int64_t bias = 1LL << (frac_bits - 1);
    return value + bias;
}

static int64_t golden_saturate(int64_t value, uint32_t narrow_width) {
    int64_t max_val = (1LL << (narrow_width - 1)) - 1;
    int64_t min_val = -(1LL << (narrow_width - 1));
    if (value > max_val) return max_val;
    if (value < min_val) return min_val;
    return value;
}

static bool golden_is_overflow(int64_t value, uint32_t narrow_width) {
    int64_t max_val = (1LL << (narrow_width - 1)) - 1;
    int64_t min_val = -(1LL << (narrow_width - 1));
    return (value > max_val) || (value < min_val);
}

static uint32_t golden_mult_width(uint32_t a_width, uint32_t b_width) {
    return a_width + b_width;
}

// NOTE: intentionally 32-bit unsigned arithmetic, wraps just like SV's
// int unsigned subtraction would if frac_bits > product_width.
static uint32_t golden_shifted_result_width(uint32_t product_width,
                                             uint32_t frac_bits,
                                             uint32_t guard_bits) {
    uint32_t diff = product_width - frac_bits;
    uint32_t floor = guard_bits + 1;
    return (diff < floor) ? floor : diff;
}

static void check_i64(const char* name, int64_t got, int64_t exp) {
    if (got != exp) {
        std::printf("FAIL [%s]: expected %lld, got %lld\n", name, (long long)exp, (long long)got);
        errors++;
    } else {
        std::printf("PASS [%s]: %lld\n", name, (long long)got);
    }
}

static void check_u32(const char* name, uint32_t got, uint32_t exp) {
    if (got != exp) {
        std::printf("FAIL [%s]: expected %u, got %u\n", name, exp, got);
        errors++;
    } else {
        std::printf("PASS [%s]: %u\n", name, got);
    }
}

static void check_bool(const char* name, bool got, bool exp) {
    if (got != exp) {
        std::printf("FAIL [%s]: expected %d, got %d\n", name, exp, got);
        errors++;
    } else {
        std::printf("PASS [%s]: %d\n", name, got);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto dut = std::make_unique<Vfp_pkg_harness>();

    auto apply = [&](int64_t value, uint32_t frac_bits, uint32_t narrow_width,
                      uint32_t a_width, uint32_t b_width,
                      uint32_t product_width, uint32_t guard_bits) {
        dut->value         = (uint64_t)value;
        dut->frac_bits      = frac_bits;
        dut->narrow_width   = narrow_width;
        dut->a_width        = a_width;
        dut->b_width        = b_width;
        dut->product_width  = product_width;
        dut->guard_bits     = guard_bits;
        dut->eval();
    };

    // Case 1: round_half_up, normal rounding (Q2.6-style, frac_bits=6)
    apply(100, 6, 24, 0, 0, 0, 0);
    check_i64("round-half-up-normal", (int64_t)dut->round_half_up_out,
              golden_round_half_up(100, 6));

    // Case 2: round_half_up, frac_bits=0 -- early-return path, value unchanged
    apply(-77, 0, 24, 0, 0, 0, 0);
    check_i64("round-half-up-zero-frac", (int64_t)dut->round_half_up_out,
              golden_round_half_up(-77, 0));

    // Case 3: round_half_up, negative value, frac_bits=1
    apply(-5, 1, 24, 0, 0, 0, 0);
    check_i64("round-half-up-negative", (int64_t)dut->round_half_up_out,
              golden_round_half_up(-5, 1));

    // Case 4: saturate, exact positive boundary (24-bit) -- no clamp
    apply(8388607, 0, 24, 0, 0, 0, 0);
    check_i64("saturate-max-boundary", (int64_t)dut->saturate_out,
              golden_saturate(8388607, 24));

    // Case 5: saturate, one past positive boundary -- clamps
    apply(8388608, 0, 24, 0, 0, 0, 0);
    check_i64("saturate-positive-overflow", (int64_t)dut->saturate_out,
              golden_saturate(8388608, 24));

    // Case 6: saturate, one past negative boundary -- clamps
    apply(-8388609, 0, 24, 0, 0, 0, 0);
    check_i64("saturate-negative-overflow", (int64_t)dut->saturate_out,
              golden_saturate(-8388609, 24));

    // Case 7: saturate, degenerate narrow_width=1 (range is [-1, 0])
    apply(0, 0, 1, 0, 0, 0, 0);
    check_i64("saturate-width1-inrange", (int64_t)dut->saturate_out,
              golden_saturate(0, 1));

    apply(1, 0, 1, 0, 0, 0, 0);
    check_i64("saturate-width1-clamps", (int64_t)dut->saturate_out,
              golden_saturate(1, 1));

    // Case 8: is_overflow, exactly at boundary -- false
    apply(32767, 0, 16, 0, 0, 0, 0);
    check_bool("is-overflow-at-boundary-false", dut->is_overflow_out,
               golden_is_overflow(32767, 16));

    // Case 9: is_overflow, one past boundary -- true
    apply(32768, 0, 16, 0, 0, 0, 0);
    check_bool("is-overflow-past-boundary-true", dut->is_overflow_out,
               golden_is_overflow(32768, 16));

    // Case 10: mult_width, typical volume_ctrl-shaped inputs (24 x 9)
    apply(0, 0, 0, 24, 9, 0, 0);
    check_u32("mult-width-typical", dut->mult_width_out, golden_mult_width(24, 9));

    // Case 11: mult_width, degenerate zero inputs
    apply(0, 0, 0, 0, 0, 0, 0);
    check_u32("mult-width-zero", dut->mult_width_out, golden_mult_width(0, 0));

    // Case 12: shifted_result_width, normal case -- diff wins over guard floor
    apply(0, 6, 0, 0, 0, 33, 1);
    check_u32("shifted-width-normal", dut->shifted_result_width_out,
              golden_shifted_result_width(33, 6, 1));

    // Case 13: shifted_result_width, guard floor wins (diff too small)
    apply(0, 6, 0, 0, 0, 8, 4);
    check_u32("shifted-width-guard-floor", dut->shifted_result_width_out,
              golden_shifted_result_width(8, 6, 4));

    // Case 14: shifted_result_width, frac_bits > product_width -- unsigned
    // wraparound path. This documents actual hardware behavior rather than
    // asserting a "sensible" result; callers must never hit this in practice.
    apply(0, 6, 0, 0, 0, 4, 1);
    check_u32("shifted-width-underflow-wraps", dut->shifted_result_width_out,
              golden_shifted_result_width(4, 6, 1));

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else              std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}