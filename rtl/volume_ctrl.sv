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

    import fp_pkg::*;

    localparam FRACTIONAL_BITS = 6;
    localparam int PRODUCT_WIDTH = mult_width(SAMPLE_WIDTH, GAIN_WIDTH + 1);

    logic signed [PRODUCT_WIDTH-1:0] product;
    logic signed [PRODUCT_WIDTH-1:0] rounded_product;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            product <= '0;
            downstream.valid <= 1'b0;
        end else begin
            downstream.valid <= upstream.valid;

            if (upstream.valid) begin
                product <= PRODUCT_WIDTH'(signed'(upstream.data)) *
                           PRODUCT_WIDTH'(signed'({1'b0, gain}));
            end
        end
    end

    assign rounded_product = product + signed'(PRODUCT_WIDTH'(1) << (FRACTIONAL_BITS - 1));
    assign downstream.data = rounded_product[SAMPLE_WIDTH+FRACTIONAL_BITS:FRACTIONAL_BITS];

endmodule