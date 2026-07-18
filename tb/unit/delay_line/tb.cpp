#include <cstdio>
#include <memory>
#include <verilated.h>
#include "Vdelay_line.h"

static int errors = 0;

static void check(const char* name, int got_data, int got_valid, int exp_data, int exp_valid) {
    if (got_data != exp_data || got_valid != exp_valid) {
        std::printf("FAIL [%s]: expected data=%d valid=%d, got data=%d valid=%d\n",
                     name, exp_data, exp_valid, got_data, got_valid);
        errors++;
    } else {
        std::printf("PASS [%s]: data=%d valid=%d\n", name, got_data, got_valid);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto dut = std::make_unique<Vdelay_line>();

    dut->clk = 0;
    dut->rst_n = 0;
    dut->data_in = 0;
    dut->valid_in = 0;

    for (int i = 0; i < 6; i++) {
        dut->clk = !dut->clk;
        if (i == 2) dut->rst_n = 1;
        dut->eval();
    }
    check("post-reset", dut->data_out, dut->valid_out, 0, 0);
    
    auto tick = [&](int data, int valid) {
        dut->clk = 0;
        dut->data_in = data;
        dut->valid_in = valid;
        dut->eval();
        dut->clk = 1;
        dut->eval();
    };

    tick(10, 1);
    check("depth2-after-push1-still-empty", dut->data_out, dut->valid_out, 0, 0);

    tick(20, 1);
    check("depth2-after-push2-first-out", dut->data_out, dut->valid_out, 10, 1);

    tick(30, 0);
    check("depth2-after-push3-second-out", dut->data_out, dut->valid_out, 20, 1);

    tick(0, 0);
    check("depth2-drains-invalid-30", dut->data_out, dut->valid_out, 30, 0);

    tick(0, 0);
    check("depth2-fully-drained", dut->data_out, dut->valid_out, 0, 0);

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else              std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}