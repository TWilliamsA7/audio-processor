#include <iostream>
#include <memory>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vtop.h"

// Number of simulation clock ticks (1 tick = half clock cycle)
#define MAX_SIM_TIME 60
vluint64_t sim_time = 0;

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    // Instantiate our design under test (DUT)
    std::unique_ptr<Vtop> dut = std::make_unique<Vtop>();

    // Enable waveform tracing
    Verilated::traceEverOn(true);
    std::unique_ptr<VerilatedVcdC> m_trace = std::make_unique<VerilatedVcdC>();
    dut->trace(m_trace.get(), 99);
    m_trace->open("waveform.vcd");

    // Initialize state
    dut->clk = 0;
    dut->rst_n = 0;
    dut->valid_in = 0;
    dut->audio_in = 0;
    dut->bypass = 0;
    dut->gain = 0x40; // 0x40 = 1.0 Unity Gain in Q1.7 fixed point

    std::cout << "Starting Audio Processor Testbench..." << std::endl;

while (sim_time < MAX_SIM_TIME) {
        // 1. Toggle Clock
        dut->clk = !dut->clk;

        // 2. Release Reset after 2 full clock cycles
        if (sim_time == 4) {
            dut->rst_n = 1;
        }

        // 3. DRIVE INPUTS ON THE FALLING EDGE (clk == 0)
        // This gives the signals half a clock cycle of setup time
        // 3. DRIVE INPUTS ON THE FALLING EDGE (clk == 0) -> Occurs on ODD sim_times
        if (!dut->clk && dut->rst_n) {
            
            // Setup for Cycle 3 rising edge (sim_time 11 is odd -> clk is 0)
            if (sim_time == 11) { 
                dut->gain = 0x40; 
                dut->audio_in = static_cast<uint32_t>(0x001000); 
                dut->valid_in = 1;
                std::cout << "[TB DEBUG] Driving audio_in = 0x" << std::hex << dut->audio_in << " at sim_time " << sim_time << std::endl;
            }
            // Clear strobe after one full cycle (2 ticks later, next falling edge)
            else if (sim_time == 13) {
                dut->valid_in = 0;
                dut->audio_in = 0;
            }

            // Setup for Cycle 6 rising edge
            else if (sim_time == 23) {
                dut->gain = 0x20; 
                dut->audio_in = static_cast<uint32_t>(0x002000); 
                dut->valid_in = 1;
                std::cout << "[TB DEBUG] Driving audio_in = 0x" << std::hex << dut->audio_in << " at sim_time " << sim_time << std::endl;
            }
            else if (sim_time == 25) {
                dut->valid_in = 0;
                dut->audio_in = 0;
            }

            // Setup for Cycle 9 rising edge
            else if (sim_time == 35) {
                dut->gain = 0x7F;         
                dut->audio_in = static_cast<uint32_t>(0x500000); 
                dut->valid_in = 1;
                std::cout << "[TB DEBUG] Driving audio_in = 0x" << std::hex << dut->audio_in << " at sim_time " << sim_time << std::endl;
            }
            else if (sim_time == 37) {
                dut->valid_in = 0;
                dut->audio_in = 0;
            }
        }

        // 4. Evaluate the hardware models
        dut->eval();

        // 5. SAMPLE OUTPUTS ON THE RISING EDGE (clk == 1)
        if (dut->clk && dut->rst_n && dut->valid_out) {
            std::cout << "[CYCLE " << (sim_time / 2) << "] "
                      << "Output Received! Data: 0x" << std::hex << dut->audio_out 
                      << std::endl;
        }

        // Write to waveform log file
        m_trace->dump(sim_time);
        sim_time++;
    }

    m_trace->close();
    std::cout << "Simulation Finished. Open waveform.vcd to inspect pipeline timing." << std::endl;
    return 0;
}