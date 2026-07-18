module volume_ctrl_harness #(
    parameter int SAMPLE_WIDTH = 8,
    parameter int GAIN_WIDTH   = 8
) (
    input  logic clk, rst_n,
    input  logic [GAIN_WIDTH-1:0] gain,
    input  logic [SAMPLE_WIDTH-1:0] in_data,
    input  logic in_valid,
    output logic [SAMPLE_WIDTH:0] out_data,  
    output logic  out_valid
);
 
    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) upstream();
    wide_stream_if  #(.WIDTH(SAMPLE_WIDTH)) downstream();
 
    assign upstream.data  = in_data;
    assign upstream.valid = in_valid;
 
    volume_ctrl #(
        .SAMPLE_WIDTH (SAMPLE_WIDTH),
        .GAIN_WIDTH   (GAIN_WIDTH)
    ) dut (
        .clk        (clk),
        .rst_n      (rst_n),
        .gain       (gain),
        .upstream   (upstream),
        .downstream (downstream)
    );
 
    assign out_data  = downstream.data;
    assign out_valid = downstream.valid;
 
endmodule