module one_pole_smoother_harness #(
    parameter int WIDTH          = 24,
    parameter int COEFF_WIDTH    = 16,
    parameter int COEFF_FRAC     = 16,
    parameter int ACC_GUARD_BITS = 2,

    localparam int ACC_WIDTH  = WIDTH + ACC_GUARD_BITS,
    localparam int DIFF_WIDTH = ACC_WIDTH + 1
) (
    input  logic clk, rst_n,
    input  logic [WIDTH-1:0]       target,
    input  logic [COEFF_WIDTH-1:0] coeff,
    input  logic                   valid_in,
    output logic [WIDTH-1:0]       state_out,
    output logic                   valid_out
);

    // diff/state_full aren't exercised by this harness's own ports -- tied
    // to named nets rather than left dangling so PINMISSING/PINCONNECTEMPTY
    // stay meaningful (same idiom as envelope_follower.sv/gate.sv).
    logic signed [DIFF_WIDTH-1:0] diff_unused;
    logic signed [ACC_WIDTH-1:0]  state_full_unused;

    one_pole_smoother #(
        .WIDTH          (WIDTH),
        .COEFF_WIDTH    (COEFF_WIDTH),
        .COEFF_FRAC     (COEFF_FRAC),
        .ACC_GUARD_BITS (ACC_GUARD_BITS)
    ) dut (
        .clk        (clk),
        .rst_n      (rst_n),
        .target     (target),
        .coeff      (coeff),
        .valid_in   (valid_in),
        .diff       (diff_unused),
        .state_full (state_full_unused),
        .state_out  (state_out),
        .valid_out  (valid_out)
    );

endmodule