module one_pole_smoother #(
    parameter int WIDTH          = 24,   // domain of target/output (unsigned)
    parameter int COEFF_WIDTH    = 16,   // unsigned Q0.COEFF_FRAC rate coefficient
    parameter int COEFF_FRAC     = 16,
    parameter int ACC_GUARD_BITS = 2,

    localparam int ACC_WIDTH     = WIDTH + ACC_GUARD_BITS,
    localparam int DIFF_WIDTH    = ACC_WIDTH + 1,
    localparam int PRODUCT_WIDTH = fp_pkg::mult_width(DIFF_WIDTH, COEFF_WIDTH)
) (
    input logic clk, rst_n,

    input logic [WIDTH-1:0]       target,
    input logic [COEFF_WIDTH-1:0] coeff,      // pre-selected by caller (attack/release, open/close, etc.)
    input logic                   valid_in,

    // diff is exposed combinationally so callers can make same-cycle
    // coefficient-select decisions (e.g. rising vs falling) off the exact
    // value this module itself uses -- not a locally recomputed copy that
    // could disagree at a state transition.
    output logic signed [DIFF_WIDTH-1:0] diff,
    output logic signed [ACC_WIDTH-1:0]  state_full,   // full-precision feedback state
    output logic [WIDTH-1:0]             state_out,    // floored-at-zero, registered
    output logic                         valid_out
);

    import fp_pkg::*;

    // Rounds, shifts by COEFF_FRAC, truncates to ACC_WIDTH -- identical
    // idiom to biquad/eq/envelope_follower's own round_shift.
    function automatic logic signed [ACC_WIDTH-1:0] round_shift(logic signed [63:0] raw);
        logic signed [63:0] rounded;
        rounded = round_half_up(raw, COEFF_FRAC);
        return ACC_WIDTH'(rounded >>> COEFF_FRAC);
    endfunction

    assign diff = DIFF_WIDTH'(signed'({1'b0, target})) - DIFF_WIDTH'(state_full);

    logic signed [PRODUCT_WIDTH-1:0] raw_product;
    logic signed [ACC_WIDTH-1:0]     sh_product;

    assign raw_product = PRODUCT_WIDTH'(diff) * PRODUCT_WIDTH'(signed'({1'b0, coeff}));
    assign sh_product  = round_shift(64'(raw_product));

    logic signed [ACC_WIDTH-1:0] state_next;
    assign state_next = state_full + sh_product;

    // Floor at zero only -- no upper saturate. Valid for any caller whose
    // target stays within [0, 2^WIDTH-1]: the convex combination cannot
    // overshoot above the target (same convexity-bound argument as the
    // original envelope_follower), so ACC_GUARD_BITS only needs to cover
    // rounding drift, not signal headroom.
    logic [WIDTH-1:0] out_next;
    assign out_next = (state_next < 0) ? '0 : state_next[WIDTH-1:0];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state_full <= '0;
            state_out  <= '0;
            valid_out  <= 1'b0;
        end else begin
            valid_out <= valid_in;
            if (valid_in) begin
                state_full <= state_next;
                state_out  <= out_next;
            end
        end
    end

endmodule