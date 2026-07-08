module audio_mux #(
    parameter SAMPLE_WIDTH = 24
) (
    input logic [SAMPLE_WIDTH-1:0] base_audio,
    input logic base_valid_in,
    input logic [SAMPLE_WIDTH-1:0] altered_audio,
    input logic altered_valid_in,
    input logic bypass,


    output logic [SAMPLE_WIDTH-1:0] audio_out,
    output logic valid_out
);

    always_comb begin
        if (bypass) begin
            audio_out = base_audio;
            valid_out = base_valid_in;
        end else begin
            audio_out = altered_audio;
            valid_out = altered_valid_in;
        end
    end
    
endmodule
