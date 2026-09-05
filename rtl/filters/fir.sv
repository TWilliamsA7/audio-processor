module fir #(
    parameter int SAMPLE_WIDTH = 24,
    parameter int COEFF_WIDTH  = 16,
    parameter int COEFF_FRAC   = 15,   // Q1.15
    parameter int GUARD_BITS   = 1,
    parameter int NUM_TAPS     = 8,

    localparam int ADDR_WIDTH = (NUM_TAPS <= 1) ? 1 : $clog2(NUM_TAPS),
    localparam int HIST_TAPS  = (NUM_TAPS > 1) ? (NUM_TAPS - 1) : 1
) (
    input logic clk, rst_n,

    // Indexed/serial coefficient loading -- one tap per cycle 
    input  logic coeff_we,
    input  logic [ADDR_WIDTH-1:0] coeff_addr,
    input  logic signed [COEFF_WIDTH-1:0] coeff_data,

    audio_stream_if.sink   upstream,
    audio_stream_if.source downstream
);

    import fp_pkg::*;

    localparam int PRODUCT_WIDTH = mult_width(SAMPLE_WIDTH, COEFF_WIDTH);
    localparam int ACC_WIDTH     = accum_width(PRODUCT_WIDTH, NUM_TAPS);
    localparam int SHIFTED_WIDTH = shifted_result_width(ACC_WIDTH, COEFF_FRAC, GUARD_BITS);

    // --- coefficient memory ---
    logic signed [COEFF_WIDTH-1:0] coeff_mem [NUM_TAPS];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < NUM_TAPS; i++) coeff_mem[i] <= '0;
        end else if (coeff_we) begin
            coeff_mem[coeff_addr] <= coeff_data;
        end
    end

    // --- tap history: x[n-1]..x[n-(NUM_TAPS-1)]; x[n] itself is
    //     upstream.data combinationally, tap 0 of the MAC below ---
    logic [SAMPLE_WIDTH-1:0] hist [HIST_TAPS];

    generate
        if (NUM_TAPS > 1) begin : g_history
            tapped_delay_line #(
                .WIDTH (SAMPLE_WIDTH),
                .TAPS  (HIST_TAPS)
            ) u_hist (
                .clk      (clk),
                .rst_n    (rst_n),
                .valid_in (upstream.valid),
                .data_in  (upstream.data),
                .data_out (hist)
            );
        end
    endgenerate

    // --- per-tap products, full precision.
    logic signed [PRODUCT_WIDTH-1:0] product [NUM_TAPS];

    genvar k;
    generate
        for (k = 0; k < NUM_TAPS; k++) begin : g_taps
            logic signed [SAMPLE_WIDTH-1:0] tap_sample;
            assign tap_sample = (k == 0) ? signed'(upstream.data) : signed'(hist[k-1]);
            assign product[k] = PRODUCT_WIDTH'(tap_sample) * PRODUCT_WIDTH'(signed'(coeff_mem[k]));
        end
    endgenerate

    // --- accumulate ---
    logic signed [ACC_WIDTH-1:0] acc_sum;
    always_comb begin
        acc_sum = '0;
        for (int i = 0; i < NUM_TAPS; i++) begin
            acc_sum = ACC_WIDTH'(acc_sum) + ACC_WIDTH'(product[i]);
        end
    end

    // --- single deferred round + shift + saturate ---
    logic signed [63:0]              rounded;
    logic signed [SHIFTED_WIDTH-1:0] shifted;
    logic signed [SAMPLE_WIDTH-1:0]  y_sat;

    assign rounded = round_half_up(64'(acc_sum), COEFF_FRAC);
    assign shifted = SHIFTED_WIDTH'(rounded >>> COEFF_FRAC);
    assign y_sat   = SAMPLE_WIDTH'(saturate(64'(shifted), SAMPLE_WIDTH));

    logic [SAMPLE_WIDTH-1:0] y_reg;
    logic reg_valid;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            y_reg     <= '0;
            reg_valid <= 1'b0;
        end else begin
            reg_valid <= upstream.valid;
            if (upstream.valid) y_reg <= y_sat;
        end
    end

    assign downstream.data  = y_reg;
    assign downstream.valid = reg_valid;

endmodule