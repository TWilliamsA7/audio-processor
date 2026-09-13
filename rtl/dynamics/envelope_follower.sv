module envelope_follower #(
    parameter int SAMPLE_WIDTH = 24,
    parameter int COEFF_WIDTH  = 16,   // unsigned Q0.COEFF_FRAC
    parameter int COEFF_FRAC   = 16,   // all-fractional -- coeff in [0,1)

    localparam int NUM_COEFFS   = 2,
    localparam int ADDR_WIDTH   = $clog2(NUM_COEFFS),
    localparam int ADDR_ATTACK  = 0,
    localparam int ADDR_RELEASE = 1,

    localparam int ACC_GUARD_BITS = 2,
    localparam int NEG_WIDTH      = SAMPLE_WIDTH + 1,
    localparam int ACC_WIDTH      = SAMPLE_WIDTH + ACC_GUARD_BITS,
    localparam int DIFF_WIDTH     = ACC_WIDTH + 1
) (
    input logic clk, rst_n,

    input logic                   coeff_we,
    input logic [ADDR_WIDTH-1:0]  coeff_addr,
    input logic [COEFF_WIDTH-1:0] coeff_data,   // unsigned

    audio_stream_if.sink   upstream,    // signed audio in
    audio_stream_if.source downstream   // unsigned envelope magnitude out
);

    // --- coefficient memory (attack/release) ---
    logic [COEFF_WIDTH-1:0] coeff_mem [NUM_COEFFS];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < NUM_COEFFS; i++) coeff_mem[i] <= '0;
        end else if (coeff_we) begin
            coeff_mem[coeff_addr] <= coeff_data;
        end
    end

    // --- rectifier: abs(x), safe at MIN_INT ---
    logic signed [SAMPLE_WIDTH-1:0] x_signed;
    logic signed [NEG_WIDTH-1:0]    x_negated;
    logic [SAMPLE_WIDTH-1:0]        abs_x;

    assign x_signed  = signed'(upstream.data);
    assign x_negated = -NEG_WIDTH'(x_signed);
    assign abs_x     = x_signed[SAMPLE_WIDTH-1] ? SAMPLE_WIDTH'(x_negated) : SAMPLE_WIDTH'(x_signed);

    // --- attack/release select, driven off the smoother's own diff ---
    logic signed [DIFF_WIDTH-1:0] diff;
    logic rising;
    logic [COEFF_WIDTH-1:0] coeff_sel;

    assign rising    = diff > 0;
    assign coeff_sel = rising ? coeff_mem[ADDR_ATTACK] : coeff_mem[ADDR_RELEASE];

    logic signed [ACC_WIDTH-1:0] smoother_state_full_unused;

    // --- shared one-pole smoothing core (also drives gate.sv's gain ramp) ---
    one_pole_smoother #(
        .WIDTH          (SAMPLE_WIDTH),
        .COEFF_WIDTH    (COEFF_WIDTH),
        .COEFF_FRAC     (COEFF_FRAC),
        .ACC_GUARD_BITS (ACC_GUARD_BITS)
    ) u_smoother (
        .clk        (clk),
        .rst_n      (rst_n),
        .target     (abs_x),
        .coeff      (coeff_sel),
        .valid_in   (upstream.valid),
        .diff       (diff),
        .state_full (smoother_state_full_unused),
        .state_out  (downstream.data),
        .valid_out  (downstream.valid)
    );

endmodule