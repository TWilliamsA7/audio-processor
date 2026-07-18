module saturator #(
    parameter SAMPLE_WIDTH = 24
) (
    input logic clk, rst_n,

    wide_stream_if.sink upstream,
    audio_stream_if.source downstream
);

    logic [SAMPLE_WIDTH-1:0] reg_data;
    logic reg_valid;

    logic is_overflow;
    logic [SAMPLE_WIDTH-1:0] sat_val;

    assign is_overflow = (upstream.data[SAMPLE_WIDTH] != upstream.data[SAMPLE_WIDTH-1]);

    assign sat_val = upstream.data[SAMPLE_WIDTH] ?
                     {1'b1, {(SAMPLE_WIDTH-1){1'b0}}} :
                     {1'b0, {(SAMPLE_WIDTH-1){1'b1}}};

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            reg_data  <= '0;
            reg_valid <= 1'b0;
        end else begin
            reg_valid <= upstream.valid;

            if (upstream.valid) begin

                reg_data <= is_overflow ? sat_val : upstream.data[SAMPLE_WIDTH-1:0];
            end
        end
    end

    assign downstream.data  = reg_data;
    assign downstream.valid = reg_valid;

endmodule