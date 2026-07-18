# tb/unit/audio_mux/Makefile

TOP = audio_mux_harness

G_PARAMS = -GSAMPLE_WIDTH=24 -GGAIN_WIDTH=8
SV_SRCS  = rtl/interfaces/audio_stream_if.sv \
		   rtl/interfaces/wide_stream_if.sv \
           rtl/audio_mux.sv \
           tb/unit/audio_mux/harness.sv

include ../unit.mk