#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>
#include <verilated.h>
#include "Vtapped_delay_line_harness.h"

static constexpr int WIDTH = 24;
static constexpr int TAPS  = 4;

static int errors = 0;

// Golden model: data_out[0] = delay 1 ... data_out[TAPS-1] = delay TAPS.
// Advances ONLY on valid_in, unlike delay_line's unconditional shift --
// that gated-hold behavior is the entire reason this primitive exists
// separately from delay_line, so it's the main thing under test here.
struct TappedDelayGolden {
    std::vector<int64_t> taps;
    TappedDelayGolden() : taps(TAPS, 0) {}

    void reset() { std::fill(taps.begin(), taps.end(), 0); }

    void tick(int64_t data_in, bool valid_in) {
        if (!valid_in) return;   // hold -- history does not absorb bubbles
        for (int i = TAPS - 1; i >= 1; i--) taps[i] = taps[i - 1];
        taps[0] = data_in;
    }
};

static int64_t sign_extend(uint64_t raw, int width) {
    int64_t v = (int64_t)raw;
    if (v & (1LL << (width - 1))) v -= (1LL << width);
    return v;
}

static void check_tap(const char* name, int tap_idx, int64_t got, int64_t exp) {
    if (got != exp) {
        std::printf("FAIL [%s] tap%d: expected %lld, got %lld\n",
                     name, tap_idx, (long long)exp, (long long)got);
        errors++;
    } else {
        std::printf("PASS [%s] tap%d: %lld\n", name, tap_idx, (long long)got);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto dut = std::make_unique<Vtapped_delay_line_harness>();
    TappedDelayGolden golden;

    dut->clk = 0;
    dut->rst_n = 0;
    dut->valid_in = 0;
    dut->data_in = 0;

    for (int i = 0; i < 6; i++) {
        dut->clk = !dut->clk;
        if (i == 2) dut->rst_n = 1;
        dut->eval();
    }
    golden.reset();

    auto check_all_taps = [&](const char* name) {
        for (int i = 0; i < TAPS; i++) {
            check_tap(name, i, sign_extend(dut->data_out[i], WIDTH), golden.taps[i]);
        }
    };

    check_all_taps("post-reset");

    auto tick = [&](int64_t data, bool valid) {
        dut->clk = 0;
        dut->data_in = (uint32_t)data & 0xFFFFFFu;
        dut->valid_in = valid;
        dut->eval();
        dut->clk = 1;
        dut->eval();
        golden.tick(data, valid);
    };

    // Case 1: continuous valid pushes -- taps should fill in delay order.
    tick(10, true);
    check_all_taps("push1");   // tap0=10, rest=0

    tick(20, true);
    check_all_taps("push2");   // tap0=20, tap1=10, rest=0

    tick(30, true);
    check_all_taps("push3");   // tap0=30, tap1=20, tap2=10, tap3=0

    tick(40, true);
    check_all_taps("push4-full");   // tap0=40, tap1=30, tap2=20, tap3=10

    // Case 2: bubble (valid=0) -- history must HOLD, not shift in garbage
    // or a zero. This is the behavior that distinguishes this primitive
    // from delay_line's unconditional shift.
    tick(999, false);
    check_all_taps("bubble-holds");   // unchanged from push4-full

    tick(999, false);
    check_all_taps("bubble-holds-again");   // still unchanged

    // Case 3: resume after bubble -- shifts from where it left off, the
    // held-during-bubble value (999) never leaks into the history.
    tick(50, true);
    check_all_taps("resume-after-bubble");   // tap0=50, tap1=40, tap2=30, tap3=20

    // Case 4: drain with continued valid pushes of zero -- confirms the
    // oldest real sample (10) ages out the far end correctly.
    tick(0, true);
    check_all_taps("drain1");   // tap0=0, tap1=50, tap2=40, tap3=30

    tick(0, true);
    check_all_taps("drain2");   // tap0=0, tap1=0, tap2=50, tap3=40

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else              std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}