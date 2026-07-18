module saturator #(
    parameter SAMPLE_WIDTH = 24
) (
    input logic clk, rst_n,

    wide_stream_if.sink upstream,
    audio_stream_if.source downstream
);

    import fp_pkg::*;

    logic [SAMPLE_WIDTH-1:0] reg_data;
    logic reg_valid;

    logic signed [SAMPLE_WIDTH:0] wide_native;
    logic signed [63:0]           wide_signed;

    assign wide_native = signed'(upstream.data);
    assign wide_signed = 64'(wide_native);

    logic [63:0] sat_val_wide;
    assign sat_val_wide = saturate(wide_signed, SAMPLE_WIDTH);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            reg_data  <= '0;
            reg_valid <= 1'b0;
        end else begin
            reg_valid <= upstream.valid;

            if (upstream.valid) begin
                reg_data <= sat_val_wide[SAMPLE_WIDTH-1:0];
            end
        end
    end

    assign downstream.data  = reg_data;
    assign downstream.valid = reg_valid;

endmodule