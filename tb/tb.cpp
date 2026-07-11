#include <iostream>
#include <fstream>
#include <memory>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vtop.h"

vluint64_t sim_time = 0;

#pragma pack(push, 1)
struct WavHeaderOut {
    char     riff_id[4]      = {'R','I','F','F'};
    uint32_t riff_size       = 0; 
    char     wave_id[4]      = {'W','A','V','E'};
    char     fmt_id[4]       = {'f','m','t',' '};
    uint32_t fmt_size        = 16;
    uint16_t audio_format    = 1; // PCM
    uint16_t num_channels    = 1;
    uint32_t sample_rate     = 0;
    uint32_t byte_rate       = 0;
    uint16_t block_align     = 0;
    uint16_t bits_per_sample = 24;
    char     data_id[4]      = {'d','a','t','a'};
    uint32_t data_size       = 0; 
};
#pragma pack(pop)


static bool find_chunk(std::ifstream& f, const char* id, uint32_t& size_out) {
    char chunk_id[4];
    uint32_t chunk_size;
    while (f.read(chunk_id, 4)) {
        if (!f.read(reinterpret_cast<char*>(&chunk_size), 4)) return false;
        if (std::memcmp(chunk_id, id, 4) == 0) {
            size_out = chunk_size;
            return true;
        }
        f.seekg(chunk_size + (chunk_size & 1), std::ios::cur);
    }
    return false;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto dut = std::make_unique<Vtop>();

    Verilated::traceEverOn(true);
    std::unique_ptr<VerilatedVcdC> m_trace = std::make_unique<VerilatedVcdC>();
    dut->trace(m_trace.get(), 99);
    m_trace->open("waveform.vcd");

    std::ifstream input_wav("input.wav", std::ios::binary);
    std::fstream output_wav("output.wav", std::ios::out | std::ios::binary | std::ios::trunc);

    if (!input_wav.is_open() || !output_wav.is_open()) {
        std::cerr << "CRITICAL: Could not open input.wav or output.wav!" << std::endl;
        return -1;
    }

    // ---- Parse RIFF/WAVE outer header ----
    char riff_id[4], wave_id[4];
    uint32_t riff_size;
    input_wav.read(riff_id, 4);
    input_wav.read(reinterpret_cast<char*>(&riff_size), 4);
    input_wav.read(wave_id, 4);
    if (!input_wav || std::memcmp(riff_id, "RIFF", 4) != 0 || std::memcmp(wave_id, "WAVE", 4) != 0) {
        std::cerr << "CRITICAL: input.wav is not a valid RIFF/WAVE file!" << std::endl;
        return -1;
    }


    uint32_t fmt_size;
    if (!find_chunk(input_wav, "fmt ", fmt_size)) {
        std::cerr << "CRITICAL: no fmt chunk found in input.wav!" << std::endl;
        return -1;
    }
    uint16_t audio_format, num_channels, block_align, bits_per_sample;
    uint32_t sample_rate, byte_rate;
    input_wav.read(reinterpret_cast<char*>(&audio_format), 2);
    input_wav.read(reinterpret_cast<char*>(&num_channels), 2);
    input_wav.read(reinterpret_cast<char*>(&sample_rate), 4);
    input_wav.read(reinterpret_cast<char*>(&byte_rate), 4);
    input_wav.read(reinterpret_cast<char*>(&block_align), 2);
    input_wav.read(reinterpret_cast<char*>(&bits_per_sample), 2);

    if (fmt_size > 16) input_wav.seekg(fmt_size - 16, std::ios::cur);

    std::cout << ">>> input.wav: " << sample_rate << " Hz, "
              << num_channels << " ch, " << bits_per_sample << "-bit PCM" << std::endl;

    if (bits_per_sample != 24 || num_channels != 1) {
        std::cerr << "CRITICAL: this testbench only handles mono 24-bit PCM. Got "
                  << num_channels << " channel(s), " << bits_per_sample << "-bit. "
                  << "Re-export input.wav as mono/24-bit, or extend the sample "
                  << "packing logic below to handle your format." << std::endl;
        return -1;
    }

    // ---- Find the "data" chunk. Read pointer now sits at the first audio byte. ----
    uint32_t data_size;
    if (!find_chunk(input_wav, "data", data_size)) {
        std::cerr << "CRITICAL: no data chunk found in input.wav!" << std::endl;
        return -1;
    }

    // ---- Write a fresh, correct placeholder header (sizes patched after streaming) ----
    WavHeaderOut hdr;
    hdr.num_channels    = 1;
    hdr.sample_rate     = sample_rate;
    hdr.bits_per_sample = 24;
    hdr.block_align     = hdr.num_channels * (hdr.bits_per_sample / 8);
    hdr.byte_rate       = hdr.sample_rate * hdr.block_align;
    output_wav.write(reinterpret_cast<char*>(&hdr), sizeof(hdr));

    // Initialize HW interface signals
    dut->clk = 0;
    dut->rst_n = 0;
    dut->valid_in = 0;
    dut->bypass = 0;
    dut->gain = 0xF0; // Apply gain boost

    // Standard hardware reset sequence
    for (int i = 0; i < 10; i++) {
        dut->clk = !dut->clk;
        if (i == 4) dut->rst_n = 1;
        dut->eval();
    }

    std::cout << ">>> Streaming audio through pipeline..." << std::endl;

    char sample_buffer[3];
    bool file_ended = false;
    int pipeline_flush_cycles = 8;

    uint32_t total_data_bytes = 0;

    while (!file_ended || pipeline_flush_cycles > 0) {
        dut->clk = 0;

        if (!file_ended) {
            if (input_wav.read(sample_buffer, 3)) {
                dut->audio_in = ((unsigned char)sample_buffer[2] << 16) |
                                ((unsigned char)sample_buffer[1] << 8)  |
                                ((unsigned char)sample_buffer[0]);
                dut->valid_in = 1;
            } else {
                file_ended = true;
                dut->audio_in = 0;
                dut->valid_in = 0;
                std::cout << ">>> File EOF reached. Flushing remaining pipeline registers..." << std::endl;
            }
        } else {
            dut->valid_in = 0;
            pipeline_flush_cycles--;
        }
        dut->eval();

        // Rising clock edge
        dut->clk = 1;
        dut->eval();

        if (dut->valid_out) {
            uint32_t data_out = dut->audio_out;
            char out_buffer[3];
            out_buffer[0] = (data_out)       & 0xFF;
            out_buffer[1] = (data_out >> 8)  & 0xFF;
            out_buffer[2] = (data_out >> 16) & 0xFF;
            output_wav.write(out_buffer, 3);
            total_data_bytes += 3;
        }

        m_trace->dump(sim_time);
        sim_time++;
    }

    m_trace->close();

    uint32_t riff_size_field = static_cast<uint32_t>(sizeof(WavHeaderOut) - 8 + total_data_bytes);
    output_wav.seekp(4);
    output_wav.write(reinterpret_cast<char*>(&riff_size_field), 4);
    output_wav.seekp(offsetof(WavHeaderOut, data_size));
    output_wav.write(reinterpret_cast<char*>(&total_data_bytes), 4);

    std::cout << ">>> Done! Total Data Bytes: " << total_data_bytes << std::endl;
    return 0;
}