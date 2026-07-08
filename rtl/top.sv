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

    // Volume to Saturator
    logic [SAMPLE_WIDTH:0] vol_to_sat_data;
    logic vol_to_sat_valid;

    // Saturator to Mux
    logic [SAMPLE_WIDTH-1:0] sat_to_mux_data;
    logic sat_to_mux_valid;

    // Stage 1: Volume Control
    volume_ctrl #(.SAMPLE_WIDTH(SAMPLE_WIDTH)) u_volume_ctrl (
        .clk(clk),
        .rst_n(rst_n),
        .gain(gain),
        .audio_in(audio_in),
        .valid_in(valid_in),
        .audio_out(vol_to_sat_data),
        .valid_out(vol_to_sat_valid)
    );

    // Stage 2: Saturation
    saturator #(.SAMPLE_WIDTH(SAMPLE_WIDTH)) u_saturator (
        .clk(clk),
        .rst_n(rst_n),
        .audio_in(vol_to_sat_data),
        .valid_in(vol_to_sat_valid),
        .audio_out(sat_to_mux_data),
        .valid_out(sat_to_mux_valid)
    );

    // Stage 3: Bypass
    audio_mux #(.SAMPLE_WIDTH(SAMPLE_WIDTH)) u_audio_mux (
        .base_audio(audio_in),
        .base_valid_in(valid_in),
        .altered_audio(sat_to_mux_data),
        .altered_valid_in(sat_to_mux_valid),
        .bypass(bypass),
        .audio_out(audio_out),
        .valid_out(valid_out)
    );

endmodule
