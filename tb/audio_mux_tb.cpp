#include <cstdio>
#include <memory>
#include <verilated.h>
#include "Vaudio_mux_harness.h"

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto dut = std::make_unique<Vaudio_mux_harness>();

    int errors = 0;
    int last_val = -1;
    int seen_bypass_0 = 0, seen_bypass_1 = 0;

    dut->clk = 0;
    dut->rst_n = 0;
    dut->stim_valid = 0;
    dut->bypass = 0;

    // Standard reset sequence, matching tb.cpp's style.
    for (int i = 0; i < 6; i++) {
        dut->clk = !dut->clk;
        if (i == 2) dut->rst_n = 1;
        dut->eval();
    }

    for (int i = 1; i <= 20; i++) {
        dut->clk = 0;
        dut->stim_data = i;
        dut->stim_valid = 1;
        dut->bypass = (i >= 10) ? 1 : 0;
        dut->eval();

        dut->clk = 1;
        dut->eval();

        if (dut->out_valid) {
            int out_data = static_cast<int>(dut->out_data);
            if (last_val != -1 && out_data != last_val + 1) {
                std::printf("FAIL: discontinuity at i=%d -- out=%d, expected %d (bypass=%d)\n",
                            i, out_data, last_val + 1, dut->bypass);
                errors++;
            } else {
                std::printf("PASS: i=%d out=%d bypass=%d\n", i, out_data, dut->bypass);
            }
            last_val = out_data;
            if (dut->bypass) seen_bypass_1++;
            else             seen_bypass_0++;
        }
    }

    dut->stim_valid = 0;
    for (int i = 0; i < 6; i++) {
        dut->clk = !dut->clk;
        dut->eval();
    }

    if (seen_bypass_0 == 0 || seen_bypass_1 == 0) {
        std::printf("FAIL: test did not exercise both bypass states (bypass=0 count=%d, bypass=1 count=%d)\n",
                    seen_bypass_0, seen_bypass_1);
        errors++;
    }

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else              std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}