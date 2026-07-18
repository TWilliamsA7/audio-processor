module volume_ctrl #(
    parameter SAMPLE_WIDTH = 24,
    parameter GAIN_WIDTH = 8
) (
    input logic clk, rst_n,

    // Gain (Q2.6 Format)
    input logic [GAIN_WIDTH-1:0] gain,

    audio_stream_if.sink upstream,
    wide_stream_if.source downstream
);

    localparam FRACTIONAL_BITS = 6;
    logic signed [SAMPLE_WIDTH+GAIN_WIDTH-1:0] product;
    logic signed [SAMPLE_WIDTH+GAIN_WIDTH-1:0] rounded_product;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            product <= '0;
            downstream.valid <= 1'b0;
        end else begin
            downstream.valid <= upstream.valid;

            if (upstream.valid) begin
                product <= 32'(signed'(upstream.data)) * 32'(signed'({1'b0, gain}));
            end
        end
    end


    assign rounded_product = product + signed'(32'd1 << (FRACTIONAL_BITS - 1));
    assign downstream.data = rounded_product[SAMPLE_WIDTH+FRACTIONAL_BITS:FRACTIONAL_BITS];

endmodule