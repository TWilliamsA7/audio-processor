# tb/unit/saturator/Makefile

TOP = saturator_harness

G_PARAMS = -GSAMPLE_WIDTH=24
SV_SRCS  = rtl/interfaces/audio_stream_if.sv \
		   rtl/interfaces/wide_stream_if.sv \
           rtl/saturator.sv \
           tb/unit/saturator/saturator_harness.sv

CPP_TB   = tb/unit/saturator/tb.cpp

include ../unit.mk