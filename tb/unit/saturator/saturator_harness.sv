module saturator_harness #(
    parameter int SAMPLE_WIDTH = 24
) (
    input  logic clk, rst_n,
    input  logic [SAMPLE_WIDTH:0] in_data, 
    input  logic in_valid,
    output logic [SAMPLE_WIDTH-1:0] out_data,
    output logic out_valid
);
 
    wide_stream_if  #(.WIDTH(SAMPLE_WIDTH)) upstream();
    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) downstream();
 
    assign upstream.data  = in_data;
    assign upstream.valid = in_valid;
 
    saturator #(
        .SAMPLE_WIDTH (SAMPLE_WIDTH)
    ) dut (
        .clk        (clk),
        .rst_n      (rst_n),
        .upstream   (upstream),
        .downstream (downstream)
    );
 
    assign out_data  = downstream.data;
    assign out_valid = downstream.valid;
 
endmodule