module saturator #(
    parameter SAMPLE_WIDTH = 24
) (
    input logic clk, rst_n,

    // Input Source
    input logic [SAMPLE_WIDTH:0] audio_in,
    input logic valid_in,

    // Output Source
    output logic [SAMPLE_WIDTH-1:0] audio_out,
    output logic valid_out

);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_out <= 1'b0;
            audio_out <= '0;
        end else begin
            valid_out <= valid_in;

            if (valid_in) begin
                // If overflow occurred
                if (audio_in[SAMPLE_WIDTH] != audio_in[SAMPLE_WIDTH-1]) begin
                    if (audio_in[SAMPLE_WIDTH] == 1'b0) begin
                        audio_out <= 24'h7FFFFF;
                    end else begin
                        audio_out <= 24'h800000;
                    end
                end else begin
                    audio_out <= audio_in[SAMPLE_WIDTH-1:0];
                end
            end

        end
    end

    
endmodule
