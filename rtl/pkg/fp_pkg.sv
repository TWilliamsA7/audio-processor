package fp_pkg;

    // --------------------------------------------------------------- //
    // Width calculations                                              //
    // --------------------------------------------------------------- //

    // Width needed to hold width signed A_WIDTH x signed B_WIDTH multiplication
    function automatic int unsigned mult_width(int unsigned a_width, int unsigned b_width);
        return a_width + b_width;
    endfunction

    // Width needed after multiplying then adding a rounding constant.
    function automatic int unsigned round_width(int unsigned product_width);
        return product_width;
    endfunction

    // Given a fixed-point value of PRODUCT_WIDTH bits with FRAC_BITS fractional
    // bits, the width of the integer part after rounding + shifting right by
    // FRAC_BITS, keeping GUARD_BITS extra headroom bits above the nominal
    // output width (e.g. for saturation logic downstream).
    function automatic int unsigned shifted_result_width(
        int unsigned product_width,
        int unsigned frac_bits,
        int unsigned guard_bits
    );
        return (product_width - frac_bits) < (guard_bits + 1) ?
               (guard_bits + 1) : (product_width - frac_bits);
    endfunction

        function automatic int unsigned accum_width(
        int unsigned product_width,
        int unsigned num_terms
    );
        return (num_terms <= 1) ? product_width : (product_width + $clog2(num_terms));
    endfunction

    // --------------------------------------------------------------- //
    // Rounding calculations                                           //
    // --------------------------------------------------------------- //


    // Round-half-up a signed fixed-point value by FRAC_BITS, returning a
    // value of the same width as the input (caller truncates/saturates
    // after). Matches the round-then-shift idiom already used in volume_ctrl.
    function automatic logic signed [63:0] round_half_up(
        logic signed [63:0] value,
        int unsigned frac_bits
    );
        logic signed [63:0] bias;
        if (frac_bits == 0) return value;
        bias = 64'sd1 <<< (frac_bits - 1);
        return value + bias;
    endfunction

    // ---------------------------------------------------------------
    // Saturation
    // ---------------------------------------------------------------

    // Saturate a signed WIDE_WIDTH-bit value down to a signed NARROW_WIDTH-bit range
    function automatic logic signed [63:0] saturate(
        logic signed [63:0] value,
        int unsigned narrow_width
    );
        logic signed [63:0] max_val;
        logic signed [63:0] min_val;
        max_val = (64'sd1 <<< (narrow_width - 1)) - 64'sd1;
        min_val = -(64'sd1 <<< (narrow_width - 1));
        if (value > max_val) return max_val;
        if (value < min_val) return min_val;
        return value;
    endfunction

    function automatic bit is_overflow(
        logic signed [63:0] value,
        int unsigned narrow_width
    );
        logic signed [63:0] max_val;
        logic signed [63:0] min_val;
        max_val = (64'sd1 <<< (narrow_width - 1)) - 64'sd1;
        min_val = -(64'sd1 <<< (narrow_width - 1));
        return (value > max_val) || (value < min_val);
    endfunction


endpackage : fp_pkg