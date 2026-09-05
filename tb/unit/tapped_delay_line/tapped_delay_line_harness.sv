module tapped_delay_line_harness #(
    parameter int WIDTH = 24,
    parameter int TAPS  = 4
) (
    input  logic clk, rst_n,
    input  logic valid_in,
    input  logic [WIDTH-1:0] data_in,
    output logic [WIDTH-1:0] data_out [TAPS]
);

    tapped_delay_line #(
        .WIDTH (WIDTH),
        .TAPS  (TAPS)
    ) dut (
        .clk      (clk),
        .rst_n    (rst_n),
        .valid_in (valid_in),
        .data_in  (data_in),
        .data_out (data_out)
    );

endmodule