# tb/unit/volume_ctrl/Makefile

TOP = volume_ctrl_harness

G_PARAMS = -GSAMPLE_WIDTH=24 -GGAIN_WIDTH=8
SV_SRCS  = rtl/interfaces/audio_stream_if.sv \
		   rtl/interfaces/wide_stream_if.sv \
           rtl/volume_ctrl.sv \
           tb/unit/volume_ctrl/volume_ctrl_harness.sv

CPP_TB = tb/unit/volume_ctrl/tb.cpp

include ../unit.mk