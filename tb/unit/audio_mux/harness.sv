module audio_mux_harness #(
    parameter int WIDTH   = 8,
    parameter int LATENCY = 2
) (
    input  logic clk, rst_n,
    input  logic bypass,
    input  logic [WIDTH-1:0] stim_data,
    input  logic stim_valid,
    output logic [WIDTH-1:0] out_data,
    output logic  out_valid
);
 
    audio_stream_if #(.WIDTH(WIDTH)) raw();
    audio_stream_if #(.WIDTH(WIDTH)) proc();
    audio_stream_if #(.WIDTH(WIDTH)) out();
 
    assign raw.data  = stim_data;
    assign raw.valid = stim_valid;
 
    delay_line #(.WIDTH(WIDTH), .DEPTH(LATENCY)) u_proc_model (
        .clk       (clk),
        .rst_n     (rst_n),
        .data_in   (stim_data),
        .valid_in  (stim_valid),
        .data_out  (proc.data),
        .valid_out (proc.valid)
    );
 
    audio_mux #(
        .SAMPLE_WIDTH (WIDTH),
        .LATENCY      (LATENCY)
    ) dut (
        .clk    (clk),
        .rst_n  (rst_n),
        .bypass (bypass),
        .raw    (raw),
        .proc   (proc),
        .out    (out)
    );
 
    assign out_data  = out.data;
    assign out_valid = out.valid;
 
endmodule