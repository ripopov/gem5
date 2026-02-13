#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <ftr/ftr_writer.h>

namespace {

constexpr uint32_t kHartsPerCore = 2;
constexpr uint64_t kCycleTimePs = 1000;
constexpr uint64_t kFibersPerHart = 2;

struct instruction_desc {
    const char* mnemonic;
    uint32_t opcode;
    bool is_load;
    bool is_store;
    uint8_t bytes;
};

const std::vector<instruction_desc> kInstructionSet = {
    {"addi", 0x13, false, false, 0},
    {"add", 0x33, false, false, 0},
    {"sub", 0x40000033, false, false, 0},
    {"xor", 0x4033, false, false, 0},
    {"and", 0x7033, false, false, 0},
    {"or", 0x6033, false, false, 0},
    {"slt", 0x2033, false, false, 0},
    {"lw", 0x2003, true, false, 4},
    {"lh", 0x1003, true, false, 2},
    {"lb", 0x3, true, false, 1},
    {"sw", 0x2023, false, true, 4},
    {"sh", 0x1023, false, true, 2},
    {"sb", 0x23, false, true, 1},
};

struct config {
    uint32_t cores = 2;
    uint64_t instructions_per_hart = 64;
    std::filesystem::path output_dir = "example/out";
};

uint64_t make_tx_id(uint32_t core_id, uint32_t hart_id, uint64_t instruction_index, bool memory_child) {
    const uint64_t core_bits = static_cast<uint64_t>(core_id) << 56;
    const uint64_t hart_bits = static_cast<uint64_t>(hart_id) << 48;
    const uint64_t inst_bits = (instruction_index & 0x0000FFFFFFFFFFFFULL) << 1;
    return core_bits | hart_bits | inst_bits | static_cast<uint64_t>(memory_child);
}

std::string hart_name(uint32_t core_id, uint32_t hart_id) {
    return "core" + std::to_string(core_id) + ".hart" + std::to_string(hart_id);
}

uint64_t fiber_index(uint32_t core_id, uint32_t hart_id) {
    return static_cast<uint64_t>(core_id) * kHartsPerCore + hart_id;
}

uint64_t pipeline_stream_id(uint32_t core_id, uint32_t hart_id) {
    return fiber_index(core_id, hart_id) * kFibersPerHart;
}

uint64_t memory_stream_id(uint32_t core_id, uint32_t hart_id) {
    return fiber_index(core_id, hart_id) * kFibersPerHart + 1;
}

uint64_t pipeline_generator_id(uint32_t core_id, uint32_t hart_id) {
    return pipeline_stream_id(core_id, hart_id);
}

uint64_t memory_generator_id(uint32_t core_id, uint32_t hart_id) {
    return memory_stream_id(core_id, hart_id);
}

[[noreturn]] void print_usage_and_exit(const char* argv0, int exit_code) {
    std::cout << "Usage: " << argv0 << " [--cores N] [--instructions N] [--out-dir DIR]\n"
              << "  --cores         Number of cores in fake SoC (default: 2)\n"
              << "  --instructions  Instructions generated per hart (default: 64)\n"
              << "  --out-dir       Output directory for .ftr files (default: example/out)\n";
    std::exit(exit_code);
}

uint64_t parse_u64(const std::string& value, const char* arg_name) {
    size_t pos = 0;
    const uint64_t out = std::stoull(value, &pos, 10);
    if(pos != value.size() || out == 0) {
        throw std::invalid_argument(std::string("Invalid value for ") + arg_name + ": " + value);
    }
    return out;
}

config parse_args(int argc, char** argv) {
    config cfg;
    for(int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if(arg == "--help" || arg == "-h") {
            print_usage_and_exit(argv[0], 0);
        }
        if((arg == "--cores" || arg == "--instructions" || arg == "--out-dir") && i + 1 >= argc) {
            throw std::invalid_argument("Missing value for " + arg);
        }
        if(arg == "--cores") {
            cfg.cores = static_cast<uint32_t>(parse_u64(argv[++i], "--cores"));
            continue;
        }
        if(arg == "--instructions") {
            cfg.instructions_per_hart = parse_u64(argv[++i], "--instructions");
            continue;
        }
        if(arg == "--out-dir") {
            cfg.output_dir = argv[++i];
            continue;
        }
        throw std::invalid_argument("Unknown argument: " + arg);
    }
    return cfg;
}

void write_hart_trace(uint32_t core_id, uint32_t hart_id, uint64_t instruction_count, ftr::ftr_writer<true>& writer) {
    const std::string hart = hart_name(core_id, hart_id);
    const uint64_t pipeline_id = pipeline_stream_id(core_id, hart_id);
    const uint64_t memory_id = memory_stream_id(core_id, hart_id);
    const uint64_t pipeline_generator = pipeline_generator_id(core_id, hart_id);
    const uint64_t memory_generator = memory_generator_id(core_id, hart_id);

    writer.writeStream(pipeline_id, hart + ".pipeline", "instruction-pipeline");
    writer.writeStream(memory_id, hart + ".memory", "memory-requests");
    writer.writeGenerator(pipeline_generator, hart + ".dispatch", pipeline_id);
    writer.writeGenerator(memory_generator, hart + ".lsu", memory_id);

    std::mt19937_64 rng(0xBADC0FFEEULL ^ (static_cast<uint64_t>(core_id) << 16) ^ hart_id);
    std::uniform_int_distribution<size_t> pick_instruction(0, kInstructionSet.size() - 1);
    std::uniform_int_distribution<uint64_t> pick_reg(0, 31);
    std::uniform_int_distribution<uint64_t> pick_stall_cycles(0, 2);
    std::uniform_int_distribution<uint64_t> pick_mem_latency_cycles(3, 12);
    std::uniform_int_distribution<uint64_t> pick_addr(0x80000000ULL, 0x8003FFFFULL);

    uint64_t cycle = 0;
    uint64_t pc = 0x80000000ULL + (static_cast<uint64_t>(core_id) << 16) + (static_cast<uint64_t>(hart_id) << 12);
    for(uint64_t i = 0; i < instruction_count; ++i) {
        const instruction_desc& instr = kInstructionSet[pick_instruction(rng)];
        const uint64_t instruction_tx_id = make_tx_id(core_id, hart_id, i, false);

        const uint64_t issue_time = cycle * kCycleTimePs;
        const uint64_t execute_time = issue_time + 2 * kCycleTimePs;
        uint64_t retire_time = issue_time + 5 * kCycleTimePs + pick_stall_cycles(rng) * kCycleTimePs;

        writer.startTransaction(instruction_tx_id, pipeline_generator, pipeline_id, issue_time);
        writer.writeAttribute(instruction_tx_id, ftr::event_type::BEGIN, "pc", ftr::data_type::UNSIGNED, pc);
        writer.writeAttribute(instruction_tx_id, ftr::event_type::BEGIN, "mnemonic", ftr::data_type::STRING, instr.mnemonic);
        writer.writeAttribute(instruction_tx_id, ftr::event_type::BEGIN, "opcode", ftr::data_type::UNSIGNED, instr.opcode);
        writer.writeAttribute(instruction_tx_id, ftr::event_type::BEGIN, "rd", ftr::data_type::UNSIGNED, pick_reg(rng));
        writer.writeAttribute(instruction_tx_id, ftr::event_type::RECORD, "rs1", ftr::data_type::UNSIGNED, pick_reg(rng));
        writer.writeAttribute(instruction_tx_id, ftr::event_type::RECORD, "rs2", ftr::data_type::UNSIGNED, pick_reg(rng));
        writer.writeAttribute(instruction_tx_id, ftr::event_type::RECORD, "execute_time_ps", ftr::data_type::TIME, execute_time);

        if(instr.is_load || instr.is_store) {
            const uint64_t memory_tx_id = make_tx_id(core_id, hart_id, i, true);
            const uint64_t memory_start = execute_time;
            const uint64_t memory_end = memory_start + pick_mem_latency_cycles(rng) * kCycleTimePs;
            const uint64_t address = pick_addr(rng) & ~(static_cast<uint64_t>(std::max<uint8_t>(instr.bytes, 1)) - 1ULL);

            writer.startTransaction(memory_tx_id, memory_generator, memory_id, memory_start);
            writer.writeAttribute(memory_tx_id, ftr::event_type::BEGIN, "kind", ftr::data_type::STRING,
                                  instr.is_load ? "load" : "store");
            writer.writeAttribute(memory_tx_id, ftr::event_type::BEGIN, "address", ftr::data_type::UNSIGNED, address);
            writer.writeAttribute(memory_tx_id, ftr::event_type::BEGIN, "size_bytes", ftr::data_type::UNSIGNED,
                                  static_cast<uint64_t>(instr.bytes));
            writer.writeAttribute(memory_tx_id, ftr::event_type::BEGIN, "core", ftr::data_type::UNSIGNED,
                                  static_cast<uint64_t>(core_id));
            writer.writeAttribute(memory_tx_id, ftr::event_type::BEGIN, "hart", ftr::data_type::UNSIGNED,
                                  static_cast<uint64_t>(hart_id));
            writer.writeAttribute(memory_tx_id, ftr::event_type::END, "completed", ftr::data_type::BOOLEAN, true);
            writer.endTransaction(memory_tx_id, memory_end);

            writer.writeRelation("child", memory_id, memory_tx_id, pipeline_id, instruction_tx_id);
            writer.writeAttribute(instruction_tx_id, ftr::event_type::RECORD, "memory_tx_id", ftr::data_type::UNSIGNED,
                                  memory_tx_id);
            writer.writeAttribute(instruction_tx_id, ftr::event_type::RECORD, "memory_wait_ps", ftr::data_type::TIME,
                                  memory_end - memory_start);
            retire_time = std::max(retire_time, memory_end + kCycleTimePs);
        }

        writer.writeAttribute(instruction_tx_id, ftr::event_type::END, "retire_time_ps", ftr::data_type::TIME, retire_time);
        writer.writeAttribute(instruction_tx_id, ftr::event_type::END, "retired", ftr::data_type::BOOLEAN, true);
        writer.endTransaction(instruction_tx_id, retire_time);

        pc += 4;
        cycle += 1 + pick_stall_cycles(rng);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const config cfg = parse_args(argc, argv);
        std::filesystem::create_directories(cfg.output_dir);
        const std::filesystem::path trace_file = cfg.output_dir / "multicore.ftr";
        ftr::ftr_writer<true> writer(trace_file.string());

        std::cout << "Generating fake RISC-V FTR traces\n";
        std::cout << "  cores: " << cfg.cores << "\n";
        std::cout << "  harts per core: " << kHartsPerCore << "\n";
        std::cout << "  instructions per hart: " << cfg.instructions_per_hart << "\n";
        std::cout << "  output directory: " << cfg.output_dir.string() << "\n";
        std::cout << "  output file: " << trace_file.string() << "\n";
        writer.writeInfo(-12);

        for(uint32_t core = 0; core < cfg.cores; ++core) {
            for(uint32_t hart = 0; hart < kHartsPerCore; ++hart) {
                write_hart_trace(core, hart, cfg.instructions_per_hart, writer);
            }
        }
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
