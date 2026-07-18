# tb/unit/audio_mux/Makefile

TOP = audio_mux_harness

G_PARAMS = -GWIDTH=8 -GLATENCY=2

SV_SRCS  = rtl/interfaces/audio_stream_if.sv \
           rtl/interfaces/wide_stream_if.sv \
           rtl/delay_line.sv \
           rtl/audio_mux.sv \
           tb/unit/audio_mux/audio_mux_harness.sv

CPP_TB   = tb/unit/audio_mux/tb.cpp

include ../unit.mk