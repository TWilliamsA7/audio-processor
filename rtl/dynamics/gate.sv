module gate #(
    parameter int SAMPLE_WIDTH = 24,
    parameter int COEFF_WIDTH  = 16,   // env attack/release AND gain-ramp rate, Q0.16
    parameter int COEFF_FRAC   = 16,
    parameter int GAIN_WIDTH   = 17,   // gain domain, Q1.16 unsigned -- unity = 1<<16
    parameter int GAIN_FRAC    = 16,

    localparam int NUM_REGS          = 6,
    localparam int ADDR_WIDTH        = $clog2(NUM_REGS),
    localparam int ADDR_ENV_ATTACK   = 0,
    localparam int ADDR_ENV_RELEASE  = 1,
    localparam int ADDR_OPEN_THRESH  = 2,
    localparam int ADDR_CLOSE_THRESH = 3,
    localparam int ADDR_OPEN_RATE    = 4,
    localparam int ADDR_CLOSE_RATE   = 5,

    // Widest loadable register is a SAMPLE_WIDTH-bit unsigned threshold;
    // rate/coeff loads use only the low COEFF_WIDTH bits of this same bus --
    // narrow-value-on-wide-bus, same idiom as eq's shared coeff bus.
    localparam int REG_DATA_WIDTH = SAMPLE_WIDTH,

    localparam int ENV_LATENCY      = 1,   // envelope_follower: 1 registered stage
    localparam int SMOOTHER_LATENCY = 1,   // one_pole_smoother: 1 registered stage
    localparam int MULT_LATENCY     = 1,   // multiply/round/saturate: 1 registered stage
    localparam int ALIGN_DELAY      = ENV_LATENCY + SMOOTHER_LATENCY,   // 2
    localparam int TOTAL_LATENCY    = ALIGN_DELAY + MULT_LATENCY        // 3
) (
    input logic clk, rst_n,

    // Shared register bus: one value per coeff_we pulse, routed by addr.
    // Generalizes eq.sv's shared-bus-plus-address-demux to registers of
    // different natural widths (thresholds vs. rate coefficients).
    input logic                      coeff_we,
    input logic [ADDR_WIDTH-1:0]     addr,
    input logic [REG_DATA_WIDTH-1:0] data,

    audio_stream_if.sink   upstream,
    audio_stream_if.source downstream
);

    import fp_pkg::*;

    // ------------------------------------------------------------------
    // Register bank: thresholds live in the same unsigned amplitude domain
    // envelope_follower's output already uses. env attack/release are NOT
    // latched here -- they're routed straight through to envelope_follower's
    // own coeff_we/addr/data ports below, same-cycle-load-uses-old-value
    // semantics preserved by that submodule itself.
    // ------------------------------------------------------------------

    logic [SAMPLE_WIDTH-1:0] open_threshold_r, close_threshold_r;
    logic [COEFF_WIDTH-1:0]  open_rate_r, close_rate_r;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            open_threshold_r  <= '0;
            close_threshold_r <= '0;
            open_rate_r       <= '0;
            close_rate_r      <= '0;
        end else if (coeff_we) begin
            case (addr)
                ADDR_WIDTH'(ADDR_OPEN_THRESH):  open_threshold_r  <= data;
                ADDR_WIDTH'(ADDR_CLOSE_THRESH): close_threshold_r <= data;
                ADDR_WIDTH'(ADDR_OPEN_RATE):    open_rate_r       <= data[COEFF_WIDTH-1:0];
                ADDR_WIDTH'(ADDR_CLOSE_RATE):   close_rate_r      <= data[COEFF_WIDTH-1:0];
                default: ; // ADDR_ENV_ATTACK / ADDR_ENV_RELEASE: handled by u_env below
            endcase
        end
    end

    logic                   env_coeff_we;
    logic                   env_coeff_addr;
    logic [COEFF_WIDTH-1:0] env_coeff_data;

    assign env_coeff_we   = coeff_we && ((addr == ADDR_WIDTH'(ADDR_ENV_ATTACK)) ||
                                          (addr == ADDR_WIDTH'(ADDR_ENV_RELEASE)));
    assign env_coeff_addr = (addr == ADDR_WIDTH'(ADDR_ENV_RELEASE));
    assign env_coeff_data = data[COEFF_WIDTH-1:0];

    // ------------------------------------------------------------------
    // Stage 1: level detection -- envelope_follower verbatim. Its own
    // attack/release let the detector react fast to onsets and slower to
    // decays, independent of the gain ramp's open/close rates below.
    // ------------------------------------------------------------------

    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) level_stream();

    envelope_follower #(
        .SAMPLE_WIDTH (SAMPLE_WIDTH),
        .COEFF_WIDTH  (COEFF_WIDTH),
        .COEFF_FRAC   (COEFF_FRAC)
    ) u_env (
        .clk        (clk),
        .rst_n      (rst_n),
        .coeff_we   (env_coeff_we),
        .coeff_addr (env_coeff_addr),
        .coeff_data (env_coeff_data),
        .upstream   (upstream),
        .downstream (level_stream)
    );

    // ------------------------------------------------------------------
    // Stage 2: hysteresis decision. gate_open_reg is the persisted state;
    // gate_open_next is the same-cycle combinational decision, fed straight
    // into the gain smoother so the ramp starts the SAME cycle a new level
    // sample crosses a threshold, not one cycle later.
    // ------------------------------------------------------------------

    logic gate_open_reg, gate_open_next;
    logic open_cond, close_cond;

    assign open_cond  = level_stream.data >= open_threshold_r;
    assign close_cond = level_stream.data <= close_threshold_r;
    assign gate_open_next = gate_open_reg ? !close_cond : open_cond;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) gate_open_reg <= 1'b0;
        else if (level_stream.valid) gate_open_reg <= gate_open_next;
    end

    // ------------------------------------------------------------------
    // Stage 3: gain ramp -- the same one_pole_smoother core as
    // envelope_follower, smoothing a target GAIN (0 or unity) instead of a
    // rectified audio magnitude. This is the whole point of the extraction.
    // ------------------------------------------------------------------

    localparam logic [GAIN_WIDTH-1:0] FULL_SCALE = GAIN_WIDTH'(1) << GAIN_FRAC;

    logic [GAIN_WIDTH-1:0]  smoother_target;
    logic [COEFF_WIDTH-1:0] smoother_coeff;

    assign smoother_target = gate_open_next ? FULL_SCALE : '0;
    assign smoother_coeff  = gate_open_next ? open_rate_r : close_rate_r;

    logic [GAIN_WIDTH-1:0] gain;
    logic                  gain_valid;

    localparam int GSM_ACC_WIDTH  = GAIN_WIDTH + 2;
    localparam int GSM_DIFF_WIDTH = GSM_ACC_WIDTH + 1;

    logic signed [GSM_DIFF_WIDTH-1:0] gain_smoother_diff_unused;
    logic signed [GSM_ACC_WIDTH-1:0]  gain_smoother_state_full_unused;

    one_pole_smoother #(
        .WIDTH          (GAIN_WIDTH),
        .COEFF_WIDTH    (COEFF_WIDTH),
        .COEFF_FRAC     (COEFF_FRAC),
        .ACC_GUARD_BITS (2)
    ) u_gain_smoother (
        .clk        (clk),
        .rst_n      (rst_n),
        .target     (smoother_target),
        .coeff      (smoother_coeff),
        .valid_in   (level_stream.valid),
        .diff       (gain_smoother_diff_unused),
        .state_full (gain_smoother_state_full_unused),
        .state_out  (gain),
        .valid_out  (gain_valid)
    );

    // ------------------------------------------------------------------
    // Stage 4: align raw audio to the gain (ALIGN_DELAY = env + smoother
    // latency), then multiply. No feedback path here, so -- like
    // volume_ctrl/FIR -- a single deferred round+shift at the output is
    // correct (the per-product-round rule only applies to recursive state).
    // ------------------------------------------------------------------

    logic [SAMPLE_WIDTH-1:0] audio_aligned;
    logic                    audio_aligned_valid;

    delay_line #(
        .WIDTH (SAMPLE_WIDTH),
        .DEPTH (ALIGN_DELAY)
    ) u_align (
        .clk       (clk),
        .rst_n     (rst_n),
        .data_in   (upstream.data),
        .valid_in  (upstream.valid),
        .data_out  (audio_aligned),
        .valid_out (audio_aligned_valid)
    );

    localparam int MULT_PRODUCT_WIDTH = mult_width(SAMPLE_WIDTH, GAIN_WIDTH + 1);

    logic signed [MULT_PRODUCT_WIDTH-1:0] product, rounded_product;

    assign product = MULT_PRODUCT_WIDTH'(signed'(audio_aligned)) *
                      MULT_PRODUCT_WIDTH'(signed'({1'b0, gain}));
    assign rounded_product = product + signed'(MULT_PRODUCT_WIDTH'(1) << (GAIN_FRAC - 1));

    logic signed [SAMPLE_WIDTH:0] mult_wide;   // guard bit, mirrors volume_ctrl
    assign mult_wide = rounded_product[SAMPLE_WIDTH+GAIN_FRAC:GAIN_FRAC];

    logic signed [SAMPLE_WIDTH-1:0] y_sat;
    assign y_sat = SAMPLE_WIDTH'(saturate(64'(mult_wide), SAMPLE_WIDTH));

    logic [SAMPLE_WIDTH-1:0] y_reg;
    logic                    y_reg_valid;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            y_reg       <= '0;
            y_reg_valid <= 1'b0;
        end else begin
            y_reg_valid <= audio_aligned_valid;
            if (audio_aligned_valid) y_reg <= y_sat;
        end
    end

    assign downstream.data  = y_reg;
    assign downstream.valid = y_reg_valid;

endmodule