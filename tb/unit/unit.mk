# tb/unit/unit.mk

ROOT := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))../../..)

VERILATOR = verilator
VERILATOR_FLAGS = --cc -Wall -Wno-UNUSED $(G_PARAMS)
OBJ_DIR = $(ROOT)/tb/unit/.obj/$(TOP)

# Expand SV_SRCS to absolute paths
ABS_SV_SRCS = $(foreach f,$(SV_SRCS),$(ROOT)/$(f))
ABS_CPP_TB  = $(ROOT)/$(CPP_TB)

.PHONY: run clean

run: $(OBJ_DIR)/$(TOP)
	@echo "--- running $(TOP) ---"
	@$(OBJ_DIR)/$(TOP) || (echo "FAILED: $(TOP)"; exit 1)

$(OBJ_DIR)/$(TOP): $(ABS_SV_SRCS) $(ABS_CPP_TB)
	@mkdir -p $(OBJ_DIR)
	$(VERILATOR) $(VERILATOR_FLAGS) --top-module $(TOP) \
		$(ABS_SV_SRCS) --exe $(ABS_CPP_TB) \
		--Mdir $(OBJ_DIR) -o $(TOP)
	$(MAKE) -s -j -C $(OBJ_DIR) -f V$(TOP).mk $(TOP)

clean:
	rm -rf $(OBJ_DIR)