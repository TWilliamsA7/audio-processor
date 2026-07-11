module audio_mux (
    input logic bypass,
    audio_stream_if.sink raw,
    audio_stream_if.sink proc,
    audio_stream_if.source out
);

    always_comb begin
        if (bypass) begin
            out.data = raw.data;
            out.valid = raw.valid;
        end else begin
            out.data = proc.data;
            out.valid = proc.valid;
        end
    end
    
endmodule
