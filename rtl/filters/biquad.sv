module biquad #(
    parameter int SAMPLE_WIDTH = 24,
    parameter int COEFF_WIDTH  = 16,
    parameter int COEFF_FRAC   = 12,   // Q4.12
    parameter int GUARD_BITS   = 1
) (
    input logic clk, rst_n,

    input logic coeff_load,
    input logic signed [COEFF_WIDTH-1:0] b0, b1, b2, a1, a2,

    audio_stream_if.sink   upstream,
    audio_stream_if.source downstream
);

    import fp_pkg::*;

    localparam int PRODUCT_WIDTH    = mult_width(SAMPLE_WIDTH, COEFF_WIDTH);                       // 40
    localparam int SHIFTED_WIDTH    = shifted_result_width(PRODUCT_WIDTH, COEFF_FRAC, GUARD_BITS);  // 28
    localparam int ACC_GUARD_BITS   = 3;   // margin for stable-filter feedback growth + internal sums
    localparam int ACC_WIDTH        = SHIFTED_WIDTH + ACC_GUARD_BITS;                               // 31
    localparam int FB_PRODUCT_WIDTH = ACC_WIDTH + COEFF_WIDTH;                                      // 47

    logic signed [COEFF_WIDTH-1:0] b0_r, b1_r, b2_r, a1_r, a2_r;
    logic signed [ACC_WIDTH-1:0]   d1_reg, d2_reg;
    logic signed [SAMPLE_WIDTH-1:0] y_reg;
    logic reg_valid;

    // Rounds, shifts by COEFF_FRAC, and truncates to ACC_WIDTH -- reused for both
    // the feedforward (x*coeff) and feedback (y_full*coeff) products, which have
    // different raw widths but share the same target accumulator width.
    function automatic logic signed [ACC_WIDTH-1:0] round_shift(logic signed [63:0] raw);
        logic signed [63:0] rounded;
        rounded = round_half_up(raw, COEFF_FRAC);
        return ACC_WIDTH'(rounded >>> COEFF_FRAC);
    endfunction

    // --- feedforward: x * b0/b1/b2, each individually rounded+shifted ---
    logic signed [PRODUCT_WIDTH-1:0] raw_b0x, raw_b1x, raw_b2x;
    logic signed [ACC_WIDTH-1:0]     sh_b0x, sh_b1x, sh_b2x;

    assign raw_b0x = PRODUCT_WIDTH'(signed'(upstream.data)) * PRODUCT_WIDTH'(signed'(b0_r));
    assign raw_b1x = PRODUCT_WIDTH'(signed'(upstream.data)) * PRODUCT_WIDTH'(signed'(b1_r));
    assign raw_b2x = PRODUCT_WIDTH'(signed'(upstream.data)) * PRODUCT_WIDTH'(signed'(b2_r));

    assign sh_b0x = round_shift(64'(raw_b0x));
    assign sh_b1x = round_shift(64'(raw_b1x));
    assign sh_b2x = round_shift(64'(raw_b2x));

    // --- state and feedback ---
    logic signed [ACC_WIDTH-1:0] y_full;
    logic signed [FB_PRODUCT_WIDTH-1:0] raw_a1y, raw_a2y;
    logic signed [ACC_WIDTH-1:0]        sh_a1y, sh_a2y;
    logic signed [ACC_WIDTH-1:0]        d1_next, d2_next;

    assign y_full = sh_b0x + d1_reg;

    assign raw_a1y = FB_PRODUCT_WIDTH'(signed'(y_full)) * FB_PRODUCT_WIDTH'(signed'(a1_r));
    assign raw_a2y = FB_PRODUCT_WIDTH'(signed'(y_full)) * FB_PRODUCT_WIDTH'(signed'(a2_r));
    assign sh_a1y  = round_shift(64'(raw_a1y));
    assign sh_a2y  = round_shift(64'(raw_a2y));

    assign d1_next = sh_b1x - sh_a1y + d2_reg;
    assign d2_next = sh_b2x - sh_a2y;

    // --- output: rounding/shifting already done per-product, so this is now just a saturate ---
    logic signed [SAMPLE_WIDTH-1:0] y_sat;
    assign y_sat = SAMPLE_WIDTH'(saturate(64'(y_full), SAMPLE_WIDTH));

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            b0_r <= '0; b1_r <= '0; b2_r <= '0; a1_r <= '0; a2_r <= '0;
        end else if (coeff_load) begin
            b0_r <= b0; b1_r <= b1; b2_r <= b2; a1_r <= a1; a2_r <= a2;
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            d1_reg <= '0; d2_reg <= '0; y_reg <= '0; reg_valid <= 1'b0;
        end else begin
            reg_valid <= upstream.valid;
            if (upstream.valid) begin
                d1_reg <= d1_next;
                d2_reg <= d2_next;
                y_reg  <= y_sat;
            end
        end
    end

    assign downstream.data  = y_reg;
    assign downstream.valid = reg_valid;

endmodule