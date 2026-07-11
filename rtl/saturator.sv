module saturator #(
    parameter SAMPLE_WIDTH = 24
) (
    input logic clk, rst_n,

    audio_stream_if.sat_sink upstream,
    audio_stream_if.source downstream
);

    // Local registers matching the standard DOWNSTREAM output width (24 bits)
    logic [SAMPLE_WIDTH-1:0] reg_data;
    logic                    reg_valid;

    // Internal routing logic variables
    logic                    is_overflow;
    logic [SAMPLE_WIDTH-1:0] sat_val;

    // 1. Detect if the true 25-bit sign (bit 24) differs from the 24-bit audio sign (bit 23)
    assign is_overflow = (upstream.sat_data[SAMPLE_WIDTH] != upstream.sat_data[SAMPLE_WIDTH-1]);
    
    // 2. Select clamp limits based on the true calculation sign bit (bit 24)
    assign sat_val = upstream.sat_data[SAMPLE_WIDTH] ? 
                     {1'b1, {(SAMPLE_WIDTH-1){1'b0}}} : // Max Negative (0x800000)
                     {1'b0, {(SAMPLE_WIDTH-1){1'b1}}};  // Max Positive (0x7FFFFF)

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            reg_data  <= '0;
            reg_valid <= 1'b0;
        end else begin
            reg_valid <= upstream.valid;
            
            if (upstream.valid) begin
                // 3. If overflowed, select the 24-bit clamp. Otherwise, cleanly slice the lower 24 bits.
                reg_data <= is_overflow ? sat_val : upstream.sat_data[SAMPLE_WIDTH-1:0];
            end
        end
    end

    // 4. Clean 1-to-1 continuous assignment straight to the output interface wire
    assign downstream.data  = reg_data;
    assign downstream.valid = reg_valid;

endmodule
