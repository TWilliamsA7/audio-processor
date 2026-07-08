module volume_ctrl #(
    parameter SAMPLE_WIDTH = 24,
    parameter GAIN_WIDTH = 8
) (
    input logic clk, rst_n,

    // Gain (Q1.7 Format)
    input logic [GAIN_WIDTH-1:0] gain,

    // Input Source
    input logic [SAMPLE_WIDTH-1:0] audio_in,
    input logic valid_in,

    // Output Sink
    output logic [SAMPLE_WIDTH:0] audio_out,
    output logic valid_out
);

    logic signed [SAMPLE_WIDTH+GAIN_WIDTH-1:0] product;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            product <= '0;
            valid_out <= 1'b0;
        end else begin
            valid_out <= valid_in;

            if (valid_in) begin
                product <= 32'(signed'(audio_in)) * 32'(signed'({1'b0, gain}));
            end
        end
    end

    assign audio_out = product[SAMPLE_WIDTH+GAIN_WIDTH-1:GAIN_WIDTH-1];
    
endmodule
