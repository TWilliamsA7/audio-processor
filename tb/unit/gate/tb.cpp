#include <cstdio>
#include <cstdint>
#include <cmath>
#include <memory>
#include <utility>
#include <deque>
#include <verilated.h>
#include "Vgate_harness.h"

// ---------------------------------------------------------------------
// Parameters (must match G_PARAMS / harness defaults).
// ---------------------------------------------------------------------
static constexpr int SAMPLE_WIDTH = 24;
static constexpr int COEFF_WIDTH  = 16;
static constexpr int COEFF_FRAC   = 16;
static constexpr int GAIN_WIDTH   = 17;
static constexpr int GAIN_FRAC    = 16;

// envelope_follower's own internal widths (SAMPLE_WIDTH=24 domain) --
// identical to envelope_follower/tb.cpp's constants.
static constexpr int ENV_ACC_GUARD_BITS = 2;
static constexpr int ENV_ACC_WIDTH      = SAMPLE_WIDTH + ENV_ACC_GUARD_BITS;   // 26
static constexpr int ENV_DIFF_WIDTH     = ENV_ACC_WIDTH + 1;                  // 27
static constexpr int ENV_PRODUCT_WIDTH  = ENV_DIFF_WIDTH + COEFF_WIDTH;       // 43

static constexpr int ADDR_ENV_ATTACK   = 0;
static constexpr int ADDR_ENV_RELEASE  = 1;
static constexpr int ADDR_OPEN_THRESH  = 2;
static constexpr int ADDR_CLOSE_THRESH = 3;
static constexpr int ADDR_OPEN_RATE    = 4;
static constexpr int ADDR_CLOSE_RATE   = 5;

// gain smoother's own internal widths (GAIN_WIDTH=17 domain).
static constexpr int GSM_ACC_GUARD_BITS = 2;
static constexpr int GSM_ACC_WIDTH      = GAIN_WIDTH + GSM_ACC_GUARD_BITS;    // 19
static constexpr int GSM_DIFF_WIDTH     = GSM_ACC_WIDTH + 1;                 // 20
static constexpr int GSM_PRODUCT_WIDTH  = GSM_DIFF_WIDTH + COEFF_WIDTH;      // 36

static constexpr int ALIGN_DELAY = 2;   // ENV_LATENCY(1) + SMOOTHER_LATENCY(1)
static constexpr uint64_t FULL_SCALE = 1ULL << GAIN_FRAC;

static int errors = 0;

// ---------------------------------------------------------------------
// Bit-level helpers (same idioms as biquad/eq/envelope_follower tb.cpp)
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
// EnvGolden -- identical to envelope_follower/tb.cpp's own golden model
// (the refactor onto one_pole_smoother is bit-exact, so this is unchanged).
// ---------------------------------------------------------------------

static int64_t env_round_shift(int64_t raw) {
    int64_t rounded = golden_round_half_up(raw, COEFF_FRAC);
    int64_t shifted = rounded >> COEFF_FRAC;
    return trunc_to_width(shifted, ENV_ACC_WIDTH);
}

struct EnvGolden {
    int64_t env_full = 0;
    int64_t env_reg  = 0;
    bool    reg_valid = false;
    uint32_t coeff_mem[2] = {0, 0};

    void reset() { env_full = 0; env_reg = 0; reg_valid = false; coeff_mem[0] = coeff_mem[1] = 0; }

    std::pair<int64_t, bool> tick(int64_t x_signed, bool valid, bool we, int addr, uint32_t data) {
        int64_t x_negated = -(int64_t)x_signed;
        uint32_t abs_x = (x_signed < 0)
                             ? (uint32_t)(x_negated & 0xFFFFFF)
                             : (uint32_t)(x_signed  & 0xFFFFFF);

        int64_t diff = trunc_to_width((int64_t)abs_x - env_full, ENV_DIFF_WIDTH);
        bool rising = diff > 0;
        uint32_t coeff_sel = rising ? coeff_mem[0] : coeff_mem[1];   // 0=attack, 1=release

        int64_t raw_product = diff * (int64_t)coeff_sel;
        int64_t sh_product  = env_round_shift(raw_product);
        int64_t env_next    = trunc_to_width(env_full + sh_product, ENV_ACC_WIDTH);
        int64_t env_out     = (env_next < 0) ? 0 : (env_next & 0xFFFFFF);

        reg_valid = valid;
        if (valid) { env_full = env_next; env_reg = env_out; }
        if (we)    { coeff_mem[addr] = data; }

        return { env_reg, reg_valid };
    }
};

// ---------------------------------------------------------------------
// GainSmootherGolden -- one_pole_smoother's math, GAIN_WIDTH=17 domain.
// ---------------------------------------------------------------------

static int64_t gsm_round_shift(int64_t raw) {
    int64_t rounded = golden_round_half_up(raw, COEFF_FRAC);
    int64_t shifted = rounded >> COEFF_FRAC;
    return trunc_to_width(shifted, GSM_ACC_WIDTH);
}

// GainSmootherGolden struct is no longer needed -- its math is inlined
// into GateGolden below, where it can correctly consume env's PRE-edge
// output rather than env's freshly-computed value.

struct GateGolden {
    EnvGolden env;

    // Gain smoother's own register (mirrors one_pole_smoother's state).
    int64_t smoother_state_full = 0;
    int64_t gain_reg = 0;
    bool    gain_valid_reg = false;

    // delay_line's two literal registers -- modeled directly, not as an
    // abstracted queue, to avoid re-introducing an extra off-by-one.
    int64_t dp0_data = 0, dp1_data = 0;
    bool    dp0_valid = false, dp1_valid = false;

    int64_t open_threshold = 0, close_threshold = 0;
    int64_t open_rate = 0, close_rate = 0;
    bool    gate_open_reg = false;

    int64_t y_reg = 0;
    bool    y_reg_valid = false;

    void reset() {
        env.reset();
        smoother_state_full = 0; gain_reg = 0; gain_valid_reg = false;
        dp0_data = dp1_data = 0; dp0_valid = dp1_valid = false;
        open_threshold = close_threshold = 0;
        open_rate = close_rate = 0;
        gate_open_reg = false;
        y_reg = 0; y_reg_valid = false;
    }

    std::pair<int64_t, bool> tick(int64_t x, bool valid, bool coeff_we, int addr, int64_t data) {
        // ---- Snapshot every register's PRE-edge value. This edge's
        // combinational logic -- everywhere a downstream register samples
        // an upstream register's output -- must use ONLY these, never a
        // value computed later in this same tick(). ----
        int64_t old_dp1_data = dp1_data;   bool old_dp1_valid = dp1_valid;
        int64_t old_gain      = gain_reg;   bool old_gain_valid = gain_valid_reg;
        int64_t old_env_reg   = env.env_reg; bool old_env_valid = env.reg_valid;
        bool    old_gate_open = gate_open_reg;
        int64_t old_open_threshold = open_threshold, old_close_threshold = close_threshold;
        int64_t old_open_rate = open_rate, old_close_rate = close_rate;

        // ---- Stage: gate's own output register, using OLD audio_aligned
        // (delay_line's pre-edge data_pipe[1]) and OLD gain. ----
        int64_t raw_product     = old_dp1_data * old_gain;
        int64_t rounded_product = raw_product + (1LL << (GAIN_FRAC - 1));
        int64_t mult_wide       = trunc_to_width(rounded_product >> GAIN_FRAC, SAMPLE_WIDTH + 1);
        int64_t y_sat           = golden_saturate(mult_wide, SAMPLE_WIDTH);

        bool    new_y_reg_valid = old_dp1_valid;
        int64_t new_y_reg       = new_y_reg_valid ? y_sat : y_reg;   // hold when invalid
        std::pair<int64_t, bool> out = { new_y_reg, new_y_reg_valid };

        // ---- envelope_follower's own register update (self-contained --
        // it already correctly uses ITS OWN pre-edge state internally). ----
        bool     env_we   = coeff_we && (addr == ADDR_ENV_ATTACK || addr == ADDR_ENV_RELEASE);
        int      env_addr = (addr == ADDR_ENV_RELEASE) ? 1 : 0;
        uint32_t env_data = (uint32_t)(data & 0xFFFF);
        env.tick(x, valid, env_we, env_addr, env_data);   // updates env's state for the NEXT call

        // ---- Hysteresis + gain smoother, using OLD env output (level as
        // it stood BEFORE this edge) -- this is the actual bug fix. ----
        bool open_cond       = old_env_reg >= old_open_threshold;
        bool close_cond      = old_env_reg <= old_close_threshold;
        bool gate_open_next  = old_gate_open ? !close_cond : open_cond;

        uint64_t smoother_target = gate_open_next ? FULL_SCALE : 0;
        uint32_t smoother_coeff  = (uint32_t)(gate_open_next ? old_open_rate : old_close_rate);

        int64_t sm_diff = trunc_to_width((int64_t)smoother_target - smoother_state_full, GSM_DIFF_WIDTH);
        int64_t sm_raw  = sm_diff * (int64_t)smoother_coeff;
        int64_t sm_sh   = gsm_round_shift(sm_raw);
        int64_t sm_next = trunc_to_width(smoother_state_full + sm_sh, GSM_ACC_WIDTH);
        int64_t sm_out  = (sm_next < 0) ? 0 : (sm_next & ((1LL << GAIN_WIDTH) - 1));

        // ---- delay_line register shift -- literal data_pipe[0]/[1]. ----
        int64_t new_dp1_data = dp0_data;  bool new_dp1_valid = dp0_valid;
        int64_t new_dp0_data = sign_extend((uint64_t)x & 0xFFFFFF, SAMPLE_WIDTH);
        bool    new_dp0_valid = valid;

        // ---- register bank ----
        if (coeff_we) {
            switch (addr) {
                case ADDR_OPEN_THRESH:  open_threshold  = data & 0xFFFFFF; break;
                case ADDR_CLOSE_THRESH: close_threshold = data & 0xFFFFFF; break;
                case ADDR_OPEN_RATE:    open_rate       = data & 0xFFFF;   break;
                case ADDR_CLOSE_RATE:   close_rate      = data & 0xFFFF;   break;
                default: break;
            }
        }

        // ---- commit all registers for the next call ----
        gate_open_reg = old_env_valid ? gate_open_next : old_gate_open;
        if (old_env_valid) { smoother_state_full = sm_next; gain_reg = sm_out; }
        gain_valid_reg = old_env_valid;
        dp0_data = new_dp0_data; dp0_valid = new_dp0_valid;
        dp1_data = new_dp1_data; dp1_valid = new_dp1_valid;
        y_reg = new_y_reg; y_reg_valid = new_y_reg_valid;

        (void)old_gain_valid;   // tracked for symmetry with other pre-edge snapshots; not separately branched on
        return out;
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
    auto dut = std::make_unique<Vgate_harness>();
    GateGolden golden;

    auto reset_dut = [&]() {
        dut->clk = 0; dut->rst_n = 0;
        dut->in_valid = 0; dut->coeff_we = 0; dut->addr = 0; dut->data = 0;
        dut->in_data = 0;
        for (int i = 0; i < 6; i++) {
            dut->clk = !dut->clk;
            if (i == 2) dut->rst_n = 1;
            dut->eval();
        }
        golden.reset();
    };

    auto tick = [&](int64_t x, bool valid, bool we, int addr, int64_t data) -> std::pair<int64_t, bool> {
        dut->clk = 0;
        dut->in_data  = (uint32_t)x & 0xFFFFFFu;
        dut->in_valid = valid;
        dut->coeff_we = we;
        dut->addr     = (uint32_t)addr;
        dut->data     = (uint32_t)data & 0xFFFFFFu;
        dut->eval();
        dut->clk = 1;
        dut->eval();
        return golden.tick(x, valid, we, addr, data);
    };
    auto dut_out = [&]() { return sign_extend(dut->out_data, SAMPLE_WIDTH); };

    // ---- Setup: fast-ish attack/release detector, moderate hysteresis,
    // moderate open/close ramp rates ----
    reset_dut();
    check("post-reset", dut_out(), dut->out_valid, 0, 0);

    const int64_t OPEN_THRESH  = 1 << 20;
    const int64_t CLOSE_THRESH = 1 << 18;   // < OPEN_THRESH -- real hysteresis gap
    const int64_t ENV_ATTACK   = quantize_coeff_q016(0.3);
    const int64_t ENV_RELEASE  = quantize_coeff_q016(0.05);
    const int64_t OPEN_RATE    = quantize_coeff_q016(0.2);
    const int64_t CLOSE_RATE   = quantize_coeff_q016(0.02);

    struct { int addr; int64_t data; const char* label; } loads[] = {
        { ADDR_ENV_ATTACK,   ENV_ATTACK,   "load-env-attack" },
        { ADDR_ENV_RELEASE,  ENV_RELEASE,  "load-env-release" },
        { ADDR_OPEN_THRESH,  OPEN_THRESH,  "load-open-thresh" },
        { ADDR_CLOSE_THRESH, CLOSE_THRESH, "load-close-thresh" },
        { ADDR_OPEN_RATE,    OPEN_RATE,    "load-open-rate" },
        { ADDR_CLOSE_RATE,   CLOSE_RATE,   "load-close-rate" },
    };
    for (auto& ld : loads) {
        auto e = tick(0, false, true, ld.addr, ld.data);
        check(ld.label, dut_out(), dut->out_valid, e.first, e.second);
    }

    // ---- Loud signal (above open threshold): gate should open, gain ramps
    // up, and after enough samples output should approach the (aligned)
    // input -- all checked bit-exact against golden throughout, not just
    // at the end. ----
    const int64_t LOUD = 1 << 22;
    for (int n = 0; n < 60; n++) {
        char label[32];
        std::snprintf(label, sizeof(label), "loud-n%d", n);
        auto e = tick(LOUD, true, false, 0, 0);
        check(label, dut_out(), dut->out_valid, e.first, e.second);
    }
    // steady-state sanity: gate should be open and passing ~full signal
    if (dut->out_valid) {
        double frac = double(dut_out()) / double(LOUD);
        if (frac < 0.85) {
            std::printf("FAIL [steady-state-open]: output only %.3f of input after 60 loud samples\n", frac);
            errors++;
        } else {
            std::printf("PASS [steady-state-open]: output %.3f of input after 60 loud samples\n", frac);
        }
    }

    // ---- Signal drops below close threshold: gate should close, gain
    // ramps down toward 0. ----
    const int64_t QUIET = 1 << 10;
    for (int n = 0; n < 150; n++) {
        char label[32];
        std::snprintf(label, sizeof(label), "quiet-n%d", n);
        auto e = tick(QUIET, true, false, 0, 0);
        check(label, dut_out(), dut->out_valid, e.first, e.second);
    }
    if (dut->out_valid) {
        double frac = std::abs(double(dut_out())) / double(LOUD);
        if (frac > 0.05) {
            std::printf("FAIL [steady-state-closed]: output still %.3f of loud-scale after 150 quiet samples\n", frac);
            errors++;
        } else {
            std::printf("PASS [steady-state-closed]: output down to %.4f of loud-scale after 150 quiet samples\n", frac);
        }
    }

    // ---- valid drop (bubble) mid-stream holds pipeline state ----
    {
        auto e1 = tick(LOUD, true, false, 0, 0);
        check("push-before-drain", dut_out(), dut->out_valid, e1.first, e1.second);
        auto e2 = tick(0, false, false, 0, 0);
        check("valid-drops-holds", dut_out(), dut->out_valid, e2.first, e2.second);
    }

    if (errors == 0) std::printf("ALL TESTS PASSED\n");
    else              std::printf("%d TEST(S) FAILED\n", errors);

    return errors == 0 ? 0 : 1;
}