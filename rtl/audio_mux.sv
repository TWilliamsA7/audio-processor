module audio_mux #(
    parameter int SAMPLE_WIDTH = 24,
    parameter int LATENCY = 2   
) (
    input logic clk, rst_n,
    input logic bypass,
    audio_stream_if.sink raw,
    audio_stream_if.sink proc,
    audio_stream_if.source out
);
 
    logic [SAMPLE_WIDTH-1:0] raw_data_aligned;
    logic raw_valid_aligned;
    logic bypass_sync;
 
    delay_line #(
        .WIDTH (SAMPLE_WIDTH),
        .DEPTH (LATENCY)
    ) u_raw_align (
        .clk       (clk),
        .rst_n     (rst_n),
        .data_in   (raw.data),
        .valid_in  (raw.valid),
        .data_out  (raw_data_aligned),
        .valid_out (raw_valid_aligned)
    );
 
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) bypass_sync <= 1'b0;
        else        bypass_sync <= bypass;
    end
 
    always_comb begin
        if (bypass_sync) begin
            out.data  = raw_data_aligned;
            out.valid = raw_valid_aligned;
        end else begin
            out.data  = proc.data;
            out.valid = proc.valid;
        end
    end
 
endmodule