module delay_line #(
    parameter int WIDTH = 24,
    parameter int DEPTH = 2
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic [WIDTH-1:0] data_in,
    input  logic             valid_in,
    output logic [WIDTH-1:0] data_out,
    output logic             valid_out
);
 
    generate
        if (DEPTH == 0) begin : g_passthrough
            assign data_out  = data_in;
            assign valid_out = valid_in;
        end else begin : g_pipeline
            logic [WIDTH-1:0] data_pipe  [DEPTH];
            logic             valid_pipe [DEPTH];
 
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    for (int i = 0; i < DEPTH; i++) begin
                        data_pipe[i]  <= '0;
                        valid_pipe[i] <= 1'b0;
                    end
                end else begin
                    data_pipe[0]  <= data_in;
                    valid_pipe[0] <= valid_in;
                    for (int i = 1; i < DEPTH; i++) begin
                        data_pipe[i]  <= data_pipe[i-1];
                        valid_pipe[i] <= valid_pipe[i-1];
                    end
                end
            end
 
            assign data_out  = data_pipe[DEPTH-1];
            assign valid_out = valid_pipe[DEPTH-1];
        end
    endgenerate
 
endmodule