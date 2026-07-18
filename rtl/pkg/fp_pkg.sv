package fp_pkg;

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

endpackage : fp_pkg