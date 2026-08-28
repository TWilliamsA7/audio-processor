module fp_pkg_harness (
    input  logic signed [63:0] value,
    input  logic [31:0]        frac_bits,
    input  logic [31:0]        narrow_width,
    input  logic [31:0]        a_width,
    input  logic [31:0]        b_width,
    input  logic [31:0]        product_width,
    input  logic [31:0]        guard_bits,

    output logic signed [63:0] round_half_up_out,
    output logic signed [63:0] saturate_out,
    output logic               is_overflow_out,
    output logic [31:0]        mult_width_out,
    output logic [31:0]        shifted_result_width_out
);

    import fp_pkg::*;

    always_comb begin
        round_half_up_out        = round_half_up(value, frac_bits);
        saturate_out              = saturate(value, narrow_width);
        is_overflow_out           = is_overflow(value, narrow_width);
        mult_width_out            = mult_width(a_width, b_width);
        shifted_result_width_out  = shifted_result_width(product_width, frac_bits, guard_bits);
    end

endmodule