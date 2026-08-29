module biquad_harness #(
    parameter int SAMPLE_WIDTH = 24,
    parameter int COEFF_WIDTH  = 16,
    parameter int COEFF_FRAC   = 12,
    parameter int GUARD_BITS   = 1
) (
    input  logic clk, rst_n,

    input  logic coeff_load,
    input  logic signed [COEFF_WIDTH-1:0] b0, b1, b2, a1, a2,

    input  logic [SAMPLE_WIDTH-1:0] in_data,
    input  logic in_valid,

    output logic [SAMPLE_WIDTH-1:0] out_data,
    output logic out_valid
);

    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) upstream();
    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) downstream();

    assign upstream.data  = in_data;
    assign upstream.valid = in_valid;

    biquad #(
        .SAMPLE_WIDTH (SAMPLE_WIDTH),
        .COEFF_WIDTH  (COEFF_WIDTH),
        .COEFF_FRAC   (COEFF_FRAC),
        .GUARD_BITS   (GUARD_BITS)
    ) dut (
        .clk        (clk),
        .rst_n      (rst_n),
        .coeff_load (coeff_load),
        .b0(b0), .b1(b1), .b2(b2), .a1(a1), .a2(a2),
        .upstream   (upstream),
        .downstream (downstream)
    );

    assign out_data  = downstream.data;
    assign out_valid = downstream.valid;

endmodule