interface audio_stream_if #(parameter WIDTH = 24);
    logic [WIDTH-1:0] data;
    logic [WIDTH:0] sat_data;
    logic             valid;

    modport sink   (input data, valid);
    modport source (output data, valid);
    modport sat_sink (input sat_data, valid);
    modport sat_source(output sat_data, valid);
endinterface
