module envelope_follower #(
    parameter int SAMPLE_WIDTH = 24,
    parameter int COEFF_WIDTH  = 16,   // unsigned Q0.COEFF_FRAC
    parameter int COEFF_FRAC   = 16,   // all-fractional -- coeff in [0,1)

    localparam int NUM_COEFFS   = 2,
    localparam int ADDR_WIDTH   = $clog2(NUM_COEFFS),
    localparam int ADDR_ATTACK  = 0,
    localparam int ADDR_RELEASE = 1
) (
    input logic clk, rst_n,

    input logic                   coeff_we,
    input logic [ADDR_WIDTH-1:0]  coeff_addr,
    input logic [COEFF_WIDTH-1:0] coeff_data,   // unsigned

    audio_stream_if.sink   upstream,    // signed audio in
    audio_stream_if.source downstream   // unsigned envelope magnitude out
);

    import fp_pkg::*;

    // --- width derivations ---
    localparam int ACC_GUARD_BITS = 2;
    localparam int ACC_WIDTH      = SAMPLE_WIDTH + ACC_GUARD_BITS;      
    localparam int ABS_WIDTH      = SAMPLE_WIDTH;                       
    localparam int NEG_WIDTH      = SAMPLE_WIDTH + 1;                   
    localparam int DIFF_WIDTH     = ACC_WIDTH + 1;                      
    localparam int PRODUCT_WIDTH  = mult_width(DIFF_WIDTH, COEFF_WIDTH); 


    function automatic logic signed [ACC_WIDTH-1:0] round_shift(logic signed [63:0] raw);
        logic signed [63:0] rounded;
        rounded = round_half_up(raw, COEFF_FRAC);
        return ACC_WIDTH'(rounded >>> COEFF_FRAC);
    endfunction

    // --- coefficient memory (attack/release) ---
    logic [COEFF_WIDTH-1:0] coeff_mem [NUM_COEFFS];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < NUM_COEFFS; i++) coeff_mem[i] <= '0;
        end else if (coeff_we) begin
            coeff_mem[coeff_addr] <= coeff_data;
        end
    end

    // --- state ---
    logic signed [ACC_WIDTH-1:0]    env_full;   // full-precision feedback state
    logic        [SAMPLE_WIDTH-1:0] env_reg;    // saturated/floored output register
    logic                           reg_valid;

    // --- rectifier: abs(x), safe at MIN_INT ---
    logic signed [SAMPLE_WIDTH-1:0] x_signed;
    logic signed [NEG_WIDTH-1:0]    x_negated;
    logic [ABS_WIDTH-1:0]           abs_x;

    assign x_signed  = signed'(upstream.data);
    assign x_negated = -NEG_WIDTH'(x_signed);
    assign abs_x     = x_signed[SAMPLE_WIDTH-1] ? ABS_WIDTH'(x_negated) : ABS_WIDTH'(x_signed);

    // --- diff against full-precision feedback state; attack/release select ---
    logic signed [DIFF_WIDTH-1:0] diff;
    logic rising;
    logic [COEFF_WIDTH-1:0] coeff_sel;

    assign diff      = DIFF_WIDTH'(signed'({1'b0, abs_x})) - DIFF_WIDTH'(env_full);
    assign rising    = diff > 0;
    assign coeff_sel = rising ? coeff_mem[ADDR_ATTACK] : coeff_mem[ADDR_RELEASE];

    // --- per-product round+shift ---
    logic signed [PRODUCT_WIDTH-1:0] raw_product;
    logic signed [ACC_WIDTH-1:0]     sh_product;

    assign raw_product = PRODUCT_WIDTH'(diff) * PRODUCT_WIDTH'(signed'({1'b0, coeff_sel}));
    assign sh_product  = round_shift(64'(raw_product));

    // --- accumulate: env_next = env_full + coeff*(abs_x - env_full) ---
    logic signed [ACC_WIDTH-1:0] env_next;
    assign env_next = env_full + sh_product;

    // --- output: floor rounding-noise negatives at zero; upper bound is
    // already guaranteed by convexity, so no upper clamp needed ---
    logic [SAMPLE_WIDTH-1:0] env_out;
    assign env_out = (env_next < 0) ? '0 : env_next[SAMPLE_WIDTH-1:0];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            env_full  <= '0;
            env_reg   <= '0;
            reg_valid <= 1'b0;
        end else begin
            reg_valid <= upstream.valid;
            if (upstream.valid) begin
                env_full <= env_next;
                env_reg  <= env_out;
            end
        end
    end

    assign downstream.data  = env_reg;
    assign downstream.valid = reg_valid;

endmodule