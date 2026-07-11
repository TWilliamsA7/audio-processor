module top #(
    parameter SAMPLE_WIDTH = 24,
    parameter GAIN_WIDTH = 8
) (
    input logic clk, rst_n,

    // Input Source
    input logic [SAMPLE_WIDTH-1:0] audio_in,
    input logic valid_in,

    // Output Sink
    output logic [SAMPLE_WIDTH-1:0] audio_out,
    output logic valid_out,

    // Control Signals
    input logic [GAIN_WIDTH-1:0] gain,
    input logic bypass
);

    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) stream_in();
    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) vol_to_sat();
    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) sat_to_mux();
    audio_stream_if #(.WIDTH(SAMPLE_WIDTH)) stream_out();

    assign stream_in.data = audio_in;
    assign stream_in.valid = valid_in;

    // Stage 1: Volume Control
    volume_ctrl #(.SAMPLE_WIDTH(SAMPLE_WIDTH)) u_volume_ctrl (
        .clk        (clk),
        .rst_n      (rst_n),
        .gain       (gain),
        .upstream   (stream_in),
        .downstream (vol_to_sat)
    );

    saturator #(.SAMPLE_WIDTH(SAMPLE_WIDTH)) u_saturator (
        .clk        (clk),
        .rst_n      (rst_n),
        .upstream   (vol_to_sat),
        .downstream (sat_to_mux)
    );

    audio_mux u_audio_mux (
        .bypass     (bypass),
        .raw     (stream_in),
        .proc    (sat_to_mux),
        .out (stream_out)
    );

    // Bind the final internal interface back to the flat output ports
    assign audio_out = stream_out.data;
    assign valid_out = stream_out.valid;

endmodule
