module gate_harness #(
    parameter int SAMPLE_WIDTH = 24,
    parameter int COEFF_WIDTH  = 16,
    parameter int COEFF_FRAC   = 16,
    parameter int GAIN_WIDTH   = 17,
    parameter int GAIN_FRAC    = 16,
    localparam int ADDR_WIDTH  = 3
) (
    input  logic clk, rst_n,

    input  logic                      coeff_we,
    input  logic [ADDR_WIDTH-1:0]     addr,
    input  logic [SAMPLE_WIDTH-1:0]   data,

    input  logic [SAMPLE_WIDTH-1:0]   in_data,
    input  logic                      in_valid,

    output logic [SAMPLE_WIDTH-1:0]   out_data,
    output logic                      out_valid
);

    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) upstream();
    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) downstream();

    assign upstream.data  = in_data;
    assign upstream.valid = in_valid;

    gate #(
        .SAMPLE_WIDTH (SAMPLE_WIDTH),
        .COEFF_WIDTH  (COEFF_WIDTH),
        .COEFF_FRAC   (COEFF_FRAC),
        .GAIN_WIDTH   (GAIN_WIDTH),
        .GAIN_FRAC    (GAIN_FRAC)
    ) dut (
        .clk        (clk),
        .rst_n      (rst_n),
        .coeff_we   (coeff_we),
        .addr       (addr),
        .data       (data),
        .upstream   (upstream),
        .downstream (downstream)
    );

    assign out_data  = downstream.data;
    assign out_valid = downstream.valid;

endmodule