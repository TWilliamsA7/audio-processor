interface wide_stream_if #(parameter WIDTH = 24, parameter GUARD_BITS = 1);
    logic [WIDTH+GUARD_BITS-1:0] data;
    logic                        valid;
 
    modport sink   (input data, valid);
    modport source (output data, valid);
endinterface