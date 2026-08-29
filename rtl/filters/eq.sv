module eq #(
    parameter int SAMPLE_WIDTH = 24,
    parameter int COEFF_WIDTH  = 16,
    parameter int COEFF_FRAC   = 12,   // Q4.12, matches biquad
    parameter int GUARD_BITS   = 1,
    parameter int NUM_BANDS    = 5,

    localparam int BAND_SEL_WIDTH = (NUM_BANDS <= 1) ? 1 : $clog2(NUM_BANDS)
) (
    input logic clk, rst_n,

    // Shared coefficient bus: one biquad's worth of coefficients, routed to
    // whichever band band_sel selects when coeff_load pulses. Mirrors the
    // narrow bus + address pattern a register file will eventually drive,
    // rather than exposing NUM_BANDS parallel coefficient ports.
    input logic coeff_load,
    input logic [BAND_SEL_WIDTH-1:0] band_sel,
    input logic signed [COEFF_WIDTH-1:0] b0, b1, b2, a1, a2,

    audio_stream_if.sink   upstream,
    audio_stream_if.source downstream
);

    // One biquad stage = one registered pipeline stage (confirmed from
    // biquad.sv: feedforward and feedback both land on the same clock edge).
    // Cascade latency is derived from that fact, not hand-counted -- whoever
    // wires this into top.sv's mux-alignment delay should reference this
    // rather than hardcoding NUM_BANDS cycles.
    localparam int STAGE_LATENCY = 1;
    localparam int TOTAL_LATENCY = NUM_BANDS * STAGE_LATENCY;

    // Internal links between consecutive biquad stages.
    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) link [NUM_BANDS-1] ();

    genvar i;
    generate
        for (i = 0; i < NUM_BANDS; i++) begin : g_band

            // This band's coefficient load is gated by band_sel -- every
            // band sees the same b0..a2 bus, only the addressed band latches
            // it this cycle.
            logic band_coeff_load;
            assign band_coeff_load = coeff_load && (band_sel == BAND_SEL_WIDTH'(i));

            if (NUM_BANDS == 1) begin : g_single
                biquad #(
                    .SAMPLE_WIDTH (SAMPLE_WIDTH),
                    .COEFF_WIDTH  (COEFF_WIDTH),
                    .COEFF_FRAC   (COEFF_FRAC),
                    .GUARD_BITS   (GUARD_BITS)
                ) u_biquad (
                    .clk        (clk),
                    .rst_n      (rst_n),
                    .coeff_load (band_coeff_load),
                    .b0(b0), .b1(b1), .b2(b2), .a1(a1), .a2(a2),
                    .upstream   (upstream),
                    .downstream (downstream)
                );
            end else if (i == 0) begin : g_first
                biquad #(
                    .SAMPLE_WIDTH (SAMPLE_WIDTH),
                    .COEFF_WIDTH  (COEFF_WIDTH),
                    .COEFF_FRAC   (COEFF_FRAC),
                    .GUARD_BITS   (GUARD_BITS)
                ) u_biquad (
                    .clk        (clk),
                    .rst_n      (rst_n),
                    .coeff_load (band_coeff_load),
                    .b0(b0), .b1(b1), .b2(b2), .a1(a1), .a2(a2),
                    .upstream   (upstream),
                    .downstream (link[0])
                );
            end else if (i == NUM_BANDS - 1) begin : g_last
                biquad #(
                    .SAMPLE_WIDTH (SAMPLE_WIDTH),
                    .COEFF_WIDTH  (COEFF_WIDTH),
                    .COEFF_FRAC   (COEFF_FRAC),
                    .GUARD_BITS   (GUARD_BITS)
                ) u_biquad (
                    .clk        (clk),
                    .rst_n      (rst_n),
                    .coeff_load (band_coeff_load),
                    .b0(b0), .b1(b1), .b2(b2), .a1(a1), .a2(a2),
                    .upstream   (link[i-1]),
                    .downstream (downstream)
                );
            end else begin : g_mid
                biquad #(
                    .SAMPLE_WIDTH (SAMPLE_WIDTH),
                    .COEFF_WIDTH  (COEFF_WIDTH),
                    .COEFF_FRAC   (COEFF_FRAC),
                    .GUARD_BITS   (GUARD_BITS)
                ) u_biquad (
                    .clk        (clk),
                    .rst_n      (rst_n),
                    .coeff_load (band_coeff_load),
                    .b0(b0), .b1(b1), .b2(b2), .a1(a1), .a2(a2),
                    .upstream   (link[i-1]),
                    .downstream (link[i])
                );
            end
        end
    endgenerate

endmodule