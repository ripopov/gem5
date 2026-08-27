/*
 * Spike (riscv-isa-sim) adapter for the gem5 RISC-V SpikeCPU
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Each instance owns one Spike processor_t together with the simif_t it runs
 * against. That simif_t hands Spike gem5's physical address space: gem5's
 * backing store is mapped straight through, so Spike's software TLB reaches
 * guest RAM with no callback at all, while every other address becomes a
 * gem5 packet on the CPU's data port. gem5 keeps ownership of simulated
 * time, of the interrupt controllers and of the devices.
 */

#include "gem5-spike.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cfg.h"
#include "decode.h"
#include "mmu.h"
#include "processor.h"
#include "simif.h"

namespace
{

/*
 * Guest instructions executed between two checks of the should_stop
 * callback. processor_t::step() cannot be interrupted from the inside, so a
 * batch is driven in chunks; this is the counterpart of QEMU's cpu_exit(),
 * which likewise takes effect only at the end of the translation block that
 * performed the MMIO access.
 */
constexpr uint64_t StepChunk = 64;

/** Pending-interrupt bits gem5 owns and pushes into the backend. */
constexpr reg_t MipMask = MIP_MSIP | MIP_MTIP | MIP_MEIP |
                          MIP_SSIP | MIP_STIP | MIP_SEIP;

/**
 * Thrown by the gem5 pseudo-instruction handler to leave step().
 *
 * The exception unwinds with state.pc still on the m5op, which is what gem5
 * expects: it executes the pseudo-instruction against its own thread context
 * and then retires it by advancing the program counter itself.
 */
struct Gem5M5OpExit
{
    uint32_t function;
};

reg_t
gem5M5Op(processor_t *, insn_t insn, reg_t)
{
    throw Gem5M5OpExit{static_cast<uint32_t>((insn.bits() >> 25) & 0x7f)};
}

/** One gem5 backing store, mapped into the guest physical address space. */
struct HostRegion
{
    uint64_t base;
    uint64_t end;
    uint8_t *host;
};

class Gem5SpikeHart final : public simif_t
{
  public:
    explicit Gem5SpikeHart(const Gem5SpikeCallbacks &callbacks);

    state_t &state() const { return *proc->get_state(); }
    processor_t &processor() const { return *proc; }

    void run(uint64_t max_instructions, Gem5SpikeRunResult *result);
    void invalidate();

    char *addr_to_mem(reg_t paddr) override;
    bool mmio_load(reg_t paddr, size_t len, uint8_t *bytes) override;
    bool mmio_store(reg_t paddr, size_t len, const uint8_t *bytes) override;
    void proc_reset(unsigned) override {}
    const cfg_t &get_cfg() const override { return cfg; }
    const std::map<size_t, processor_t *> &
    get_harts() const override
    {
        return harts;
    }
    const char *get_symbol(uint64_t) override { return nullptr; }

  private:
    const Gem5SpikeCallbacks callbacks;
    const std::string isaString;
    const std::string privilegeString;
    cfg_t cfg;
    std::vector<HostRegion> regions;
    std::unique_ptr<processor_t> proc;
    std::map<size_t, processor_t *> harts;

    /** Set by an MMIO access, which may have scheduled a gem5 event. */
    bool ioAccessed = false;

    bool
    shouldStop() const
    {
        return ioAccessed || (callbacks.should_stop &&
                              callbacks.should_stop(callbacks.opaque));
    }
};

Gem5SpikeHart::Gem5SpikeHart(const Gem5SpikeCallbacks &cb)
    : callbacks(cb),
      isaString(cb.isa_string ? cb.isa_string : "rv64imafdc_zicsr_zifencei"),
      privilegeString(cb.privilege_string ? cb.privilege_string : "MSU")
{
    debug_mmu = nullptr;

    cfg.isa = isaString.c_str();
    cfg.priv = privilegeString.c_str();
    cfg.hartids = std::vector<size_t>({static_cast<size_t>(cb.hart_id)});
    cfg.explicit_hartids = true;

    // Collect the directly mapped regions before the processor exists: the
    // software TLB refills from them without entering gem5 at all, which is
    // where a functional backend gets its speed.
    if (callbacks.memory_map) {
        uint64_t base = 0;
        uint64_t size = 0;
        uint8_t *host = nullptr;
        int writable = 0;
        for (size_t index = 0;
             callbacks.memory_map(callbacks.opaque, index, &base, &size,
                                  &host, &writable) == 0;
             ++index) {
            regions.push_back(HostRegion{base, base + size, host});
        }
    }

    proc = std::make_unique<processor_t>(
        isaString.c_str(), privilegeString.c_str(), &cfg, this,
        static_cast<uint32_t>(cb.hart_id), /* halt_on_reset */ false,
        /* log_file */ nullptr, std::cerr);

    // Spike normally learns the supported translation modes from the device
    // tree; gem5 reports them from its own ISA object instead.
    proc->set_max_vaddr_bits(cb.max_vaddr_bits);
    proc->reset();

    insn_desc_t m5op{};
    m5op.match = 0x0000007bULL;
    m5op.mask = 0x0000007fULL;
    m5op.fast_rv32i = &gem5M5Op;
    m5op.fast_rv64i = &gem5M5Op;
    m5op.fast_rv32e = &gem5M5Op;
    m5op.fast_rv64e = &gem5M5Op;
    m5op.logged_rv32i = &gem5M5Op;
    m5op.logged_rv64i = &gem5M5Op;
    m5op.logged_rv32e = &gem5M5Op;
    m5op.logged_rv64e = &gem5M5Op;
    proc->register_custom_insn(m5op);
    proc->build_opcode_map();

    harts[static_cast<size_t>(cb.hart_id)] = proc.get();
}

char *
Gem5SpikeHart::addr_to_mem(reg_t paddr)
{
    for (const HostRegion &region : regions) {
        if (paddr >= region.base && paddr < region.end) {
            return reinterpret_cast<char *>(region.host +
                                            (paddr - region.base));
        }
    }
    return nullptr;
}

bool
Gem5SpikeHart::mmio_load(reg_t paddr, size_t len, uint8_t *bytes)
{
    if (callbacks.memory_read(callbacks.opaque, paddr, bytes, len) != 0) {
        return false;
    }
    ioAccessed = true;
    return true;
}

bool
Gem5SpikeHart::mmio_store(reg_t paddr, size_t len, const uint8_t *bytes)
{
    if (callbacks.memory_write(callbacks.opaque, paddr, bytes, len) != 0) {
        return false;
    }
    ioAccessed = true;
    return true;
}

void
Gem5SpikeHart::run(uint64_t max_instructions, Gem5SpikeRunResult *result)
{
    std::memset(result, 0, sizeof(*result));
    result->reason = GEM5_SPIKE_EXIT_BUDGET;
    ioAccessed = false;

    if (callbacks.run_begin) {
        callbacks.run_begin(callbacks.opaque);
    }

    // gem5's mtime is constant for the whole batch: the batch executes at one
    // simulated tick and time advances only once it returns. Sampling it once
    // here is therefore equivalent to servicing every rdtime individually.
    if (callbacks.read_time) {
        state().time->sync(callbacks.read_time(callbacks.opaque));
    }

    state_t &s = state();
    // minstret is how many instructions Spike reports having retired. A guest
    // that inhibits it would freeze that count, so fall back to single
    // stepping, where each step is one instruction by construction.
    const bool inhibited = s.mcountinhibit->read() & MCOUNTINHIBIT_IR;
    const uint64_t chunkLimit = inhibited ? 1 : StepChunk;

    uint64_t executed = 0;
    try {
        while (executed < max_instructions) {
            if (shouldStop()) {
                break;
            }
            const uint64_t chunk =
                std::min(max_instructions - executed, chunkLimit);
            const reg_t before = s.minstret->read();
            proc->step(chunk);
            uint64_t retired = s.minstret->read() - before;
            // A step that retires nothing still took a trap or sat in WFI,
            // and gem5 must be told that something happened.
            retired = std::clamp<uint64_t>(retired, 1, chunk);
            executed += retired;

            // Test WFI only after stepping. Nothing but step() takes a
            // pending interrupt, so a hart that is asked whether it is
            // halted before it is given the chance to wake would never
            // wake at all.
            if (proc->is_waiting_for_interrupt()) {
                break;
            }
        }
    } catch (Gem5M5OpExit &m5op) {
        result->reason = GEM5_SPIKE_EXIT_M5OP;
        result->m5_function = m5op.function;
    } catch (std::exception &error) {
        std::cerr << "gem5-spike: " << error.what() << std::endl;
        result->reason = GEM5_SPIKE_EXIT_ERROR;
    }

    if (result->reason == GEM5_SPIKE_EXIT_BUDGET &&
        proc->is_waiting_for_interrupt()) {
        result->reason = GEM5_SPIKE_EXIT_HALTED;
    }
    result->instructions = executed;

    if (callbacks.run_end) {
        callbacks.run_end(callbacks.opaque);
    }
}

void
Gem5SpikeHart::invalidate()
{
    proc->get_mmu()->flush_tlb();
    proc->get_mmu()->yield_load_reservation();
    proc->clear_waiting_for_interrupt();
}

std::map<uint32_t, std::unique_ptr<Gem5SpikeHart>> instances;

Gem5SpikeHart *
hart(uint32_t instance_id)
{
    const auto found = instances.find(instance_id);
    return found == instances.end() ? nullptr : found->second.get();
}

} // namespace

int
gem5_spike_init(const Gem5SpikeCallbacks *callbacks)
{
    if (!callbacks || !callbacks->memory_read || !callbacks->memory_write ||
        instances.count(callbacks->instance_id)) {
        return -1;
    }
    instances.emplace(callbacks->instance_id,
                      std::make_unique<Gem5SpikeHart>(*callbacks));
    return 0;
}

int
gem5_spike_run(uint32_t instance_id, uint64_t max_instructions,
               Gem5SpikeRunResult *result)
{
    Gem5SpikeHart *instance = hart(instance_id);
    if (!instance || !result) {
        return -1;
    }
    instance->run(max_instructions, result);
    return 0;
}

uint64_t
gem5_spike_get_gpr(uint32_t instance_id, unsigned index)
{
    Gem5SpikeHart *instance = hart(instance_id);
    return instance && index < NXPR ? instance->state().XPR[index] : 0;
}

int
gem5_spike_set_gpr(uint32_t instance_id, unsigned index, uint64_t value)
{
    Gem5SpikeHart *instance = hart(instance_id);
    if (!instance || index >= NXPR) {
        return -1;
    }
    instance->state().XPR.write(index, value);
    return 0;
}

uint64_t
gem5_spike_get_fpr(uint32_t instance_id, unsigned index)
{
    Gem5SpikeHart *instance = hart(instance_id);
    return instance && index < NFPR ? instance->state().FPR[index].v[0] : 0;
}

int
gem5_spike_set_fpr(uint32_t instance_id, unsigned index, uint64_t value)
{
    Gem5SpikeHart *instance = hart(instance_id);
    if (!instance || index >= NFPR) {
        return -1;
    }
    // gem5 keeps the NaN-boxed 64-bit value; Spike widens it to its 128-bit
    // float register, boxed the same way its own instructions box results.
    freg_t boxed;
    boxed.v[0] = value;
    boxed.v[1] = static_cast<uint64_t>(-1);
    instance->state().FPR.write(index, boxed);
    return 0;
}

uint64_t
gem5_spike_get_pc(uint32_t instance_id)
{
    Gem5SpikeHart *instance = hart(instance_id);
    return instance ? instance->state().pc : 0;
}

void
gem5_spike_set_pc(uint32_t instance_id, uint64_t value)
{
    Gem5SpikeHart *instance = hart(instance_id);
    if (instance) {
        instance->state().pc = value;
    }
}

unsigned
gem5_spike_get_priv(uint32_t instance_id)
{
    Gem5SpikeHart *instance = hart(instance_id);
    return instance ? instance->state().prv : 0;
}

int
gem5_spike_set_priv(uint32_t instance_id, unsigned value)
{
    Gem5SpikeHart *instance = hart(instance_id);
    if (!instance) {
        return -1;
    }
    // set_privilege() flushes the software TLB and the decode cache, so do
    // not pay for it when the mode is not actually moving.
    if (instance->state().prv != value) {
        instance->processor().set_privilege(value, false);
    }
    return 0;
}

int
gem5_spike_get_csr(uint32_t instance_id, unsigned csr, uint64_t *value)
{
    Gem5SpikeHart *instance = hart(instance_id);
    if (!instance || !value) {
        return -1;
    }
    // Read the CSR object directly: processor_t::get_csr() throws an illegal
    // instruction trap for a CSR this configuration does not implement, and
    // a state transfer is not an instruction.
    const auto &csrmap = instance->state().csrmap;
    const auto found = csrmap.find(csr);
    if (found == csrmap.end()) {
        return -1;
    }
    *value = found->second->read();
    return 0;
}

int
gem5_spike_set_csr(uint32_t instance_id, unsigned csr, uint64_t value)
{
    Gem5SpikeHart *instance = hart(instance_id);
    if (!instance || !instance->state().csrmap.count(csr)) {
        return -1;
    }
    instance->processor().put_csr(csr, value);
    return 0;
}

uint64_t
gem5_spike_get_mip(uint32_t instance_id)
{
    Gem5SpikeHart *instance = hart(instance_id);
    return instance ? instance->state().mip->read() : 0;
}

void
gem5_spike_set_mip(uint32_t instance_id, uint64_t value)
{
    Gem5SpikeHart *instance = hart(instance_id);
    if (instance) {
        instance->state().mip->backdoor_write_with_mask(MipMask, value);
    }
}

void
gem5_spike_invalidate_translations(uint32_t instance_id)
{
    Gem5SpikeHart *instance = hart(instance_id);
    if (instance) {
        instance->invalidate();
    }
}
