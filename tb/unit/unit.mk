# tb/unit/unit.mk

ROOT := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))../..)

VERILATOR       = verilator
VERILATOR_FLAGS = --cc -Wall -Wno-UNUSED $(G_PARAMS)
OBJ_DIR         = $(ROOT)/tb/unit/.obj/$(TOP)

# SV_SRCS: plain .sv files, expanded to absolute paths
ABS_SV_SRCS = $(foreach f,$(SV_SRCS),$(ROOT)/$(f))

# F_FILES: .f file list references, expanded to "-f /abs/path/to/x.f"
ABS_F_FILES = $(foreach f,$(F_FILES),-f $(ROOT)/$(f))

ABS_CPP_TB  = $(ROOT)/$(CPP_TB)

.PHONY: run clean

run: $(OBJ_DIR)/$(TOP)
	@echo "--- running $(TOP) ---"
	@$(OBJ_DIR)/$(TOP) || (echo "FAILED: $(TOP)"; exit 1)

$(OBJ_DIR)/$(TOP): $(ABS_CPP_TB)
	@mkdir -p $(OBJ_DIR)
	cd $(ROOT) && $(VERILATOR) $(VERILATOR_FLAGS) --top-module $(TOP) \
		$(ABS_F_FILES) $(ABS_SV_SRCS) \
		--exe $(ABS_CPP_TB) \
		--Mdir $(OBJ_DIR) -o $(TOP)
	$(MAKE) -s -j -C $(OBJ_DIR) -f V$(TOP).mk $(TOP)

clean:
	rm -rf $(OBJ_DIR)