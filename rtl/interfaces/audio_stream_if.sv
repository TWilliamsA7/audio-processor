interface audio_stream_if #(parameter WIDTH = 24);
    logic [WIDTH-1:0] data;
    logic              valid;
 
    modport sink   (input data, valid);
    modport source (output data, valid);
endinterface
