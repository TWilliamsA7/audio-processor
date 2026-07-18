# Makefile
VERILATOR = verilator
VERILATOR_FLAGS = -Wall --cc --trace -Wno-UNUSED --top-module top

# Explicitly list the files (top must be the primary module)
SV_FILES = -f rtl/modules.f
CPP_FILES = tb/tb.cpp
TARGET = Vtop

.PHONY: all build simulate clean test

all: simulate

build:
	@echo "Building simulation..."
	$(VERILATOR) $(VERILATOR_FLAGS) $(SV_FILES) --exe $(CPP_FILES)
	make -j -C obj_dir -f $(TARGET).mk $(TARGET)

simulate: build
	@echo "Running simulation..."
	./obj_dir/$(TARGET)

	.PHONY: all build simulate clean test

test:
	@for d in tb/unit/*/; do \
		$(MAKE) --no-print-directory -C $$d run || exit 1; \
	done

clean:
	@echo "Cleaning build..."
	rm -rf obj_dir *.vcd