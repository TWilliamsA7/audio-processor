# Makefile
VERILATOR = verilator
VERILATOR_FLAGS = -Wall --cc --trace

# Explicitly list the files (top must be the primary module)
SV_FILES = -f rtl/files.f
CPP_FILES = tb/tb.cpp
TARGET = Vtop

all: simulate

build:
	@echo "Building simulation..."
	$(VERILATOR) $(VERILATOR_FLAGS) $(SV_FILES) --exe $(CPP_FILES)
	make -j -C obj_dir -f $(TARGET).mk $(TARGET)

simulate: build
	@echo "Running simulation..."
	./obj_dir/$(TARGET)

clean:
	@echo "Cleaning build..."
	rm -rf obj_dir *.vcd