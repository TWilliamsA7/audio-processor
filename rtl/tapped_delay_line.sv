module tapped_delay_line #(
    parameter int WIDTH = 24,
    parameter int TAPS  = 4    // exposes delays 1..TAPS (x[n-1] .. x[n-TAPS])
) (
    input logic clk, rst_n,
    input logic valid_in, 
    input  logic [WIDTH-1:0] data_in,
    output logic [WIDTH-1:0] data_out [TAPS]
);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < TAPS; i++) data_out[i] <= '0;
        end else if (valid_in) begin
            data_out[0] <= data_in;
            for (int i = 1; i < TAPS; i++) data_out[i] <= data_out[i-1];
        end
    end

endmodule