/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/ramulator2.hh"

#include <fstream>

#include "base/callback.hh"
#include "base/output.hh"
#include "base/trace.hh"
#include "debug/Drain.hh"
#include "debug/Ramulator2.hh"
#include "sim/system.hh"

#pragma push_macro("warn")
#undef warn

#include "ramulator/base/base.h"
#include "ramulator/base/config.h"
#include "ramulator/base/factory.h"
#include "ramulator/base/request.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/i_memory_system.h"

#pragma pop_macro("warn")

namespace gem5
{

namespace memory
{

Ramulator2::Ramulator2(const Params& p) :
    AbstractMemory(p),
    port(name() + ".port", *this),
    ramulatorConfig(p.ramulator_config),
    frontend(nullptr),
    memorySystem(nullptr),
    retryReq(false),
    retryResp(false),
    startTick(0),
    nbrOutstandingReads(0),
    nbrOutstandingWrites(0),
    sendResponseEvent([this]{ sendResponse(); }, name()),
    tickEvent([this]{ tick(); }, name())
{
    registerExitCallback([this]() {
        if (!frontend || !memorySystem) {
            return;
        }

        frontend->finalize();
        memorySystem->finalize();

        std::ofstream statsFile(simout.resolve("ramulator_stats.yaml"));
        frontend->print_stats(statsFile);
        memorySystem->print_stats(statsFile);
    });
}

Ramulator2::~Ramulator2()
{
    delete frontend;
    delete memorySystem;
}

void
Ramulator2::init()
{
    AbstractMemory::init();

    if (!port.isConnected()) {
        fatal("Ramulator2 %s is unconnected!\n", name());
    }

    if (ramulatorConfig.empty()) {
        fatal("Ramulator2 %s has an empty ramulator_config\n", name());
    }

    Ramulator::ConfigNode config =
        Ramulator::Config::parse_config_string(ramulatorConfig);
    frontend = Ramulator::Factory::create_frontend(config);
    memorySystem = Ramulator::Factory::create_memory_system(config);

    frontend->connect_memory_system(memorySystem);
    memorySystem->connect_frontend(frontend);

    if (system()->cacheLineSize() != memorySystem->get_tx_bytes()) {
        fatal("Ramulator2 transaction size %d does not match cache line "
              "size %d\n", memorySystem->get_tx_bytes(),
              system()->cacheLineSize());
    }

    port.sendRangeChange();
}

void
Ramulator2::startup()
{
    startTick = curTick();
    schedule(tickEvent, clockEdge());
}

void
Ramulator2::resetStats()
{
    if (frontend && memorySystem) {
        frontend->reset_stats_recursive();
        memorySystem->reset_stats_recursive();
    }
}

void
Ramulator2::sendResponse()
{
    assert(!retryResp);
    assert(!responseQueue.empty());

    if (port.sendTimingResp(responseQueue.front())) {
        responseQueue.pop_front();

        if (!responseQueue.empty() && !sendResponseEvent.scheduled()) {
            schedule(sendResponseEvent, curTick());
        }

        if (nbrOutstanding() == 0) {
            signalDrainDone();
        }
    } else {
        retryResp = true;
        assert(!sendResponseEvent.scheduled());
    }
}

unsigned int
Ramulator2::nbrOutstanding() const
{
    return nbrOutstandingReads + nbrOutstandingWrites +
        responseQueue.size();
}

void
Ramulator2::tick()
{
    if (system()->isTimingMode()) {
        memorySystem->tick();

        if (retryReq) {
            retryReq = false;
            port.sendRetryReq();
        }
    }

    schedule(tickEvent,
        curTick() + memorySystem->get_tCK() * sim_clock::as_float::ns);
}

Tick
Ramulator2::recvAtomic(PacketPtr pkt)
{
    panic_if(pkt->cacheResponding(),
             "Ramulator2 should not receive cache-responding packets");
    access(pkt);
    return 50000;
}

void
Ramulator2::recvFunctional(PacketPtr pkt)
{
    pkt->pushLabel(name());
    functionalAccess(pkt);

    for (auto i = responseQueue.begin(); i != responseQueue.end(); ++i) {
        pkt->trySatisfyFunctional(*i);
    }

    pkt->popLabel();
}

bool
Ramulator2::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(Ramulator2, "recvTimingReq: request %s addr %#x size %d\n",
            pkt->cmdString(), pkt->getAddr(), pkt->getSize());

    panic_if(pkt->cacheResponding(),
             "Ramulator2 should not receive cache-responding packets");
    panic_if(!(pkt->isRead() || pkt->isWrite()),
             "Ramulator2 only supports reads and writes; saw %s to %#llx\n",
             pkt->cmdString(), pkt->getAddr());

    if (retryReq) {
        return false;
    }

    bool enqueueSuccess = false;
    if (pkt->isRead()) {
        enqueueSuccess = frontend->receive_external_requests(
            Ramulator::Request::Type::Read,
            pkt->getAddr(),
            0,
            [this](Ramulator::Request& req) {
                auto it = outstandingReads.find(req.addr);
                panic_if(it == outstandingReads.end(),
                         "No outstanding Ramulator2 read for %#llx\n",
                         req.addr);

                PacketPtr pkt = it->second.front();
                it->second.pop_front();
                if (it->second.empty()) {
                    outstandingReads.erase(it);
                }

                --nbrOutstandingReads;
                accessAndRespond(pkt);
            },
            pkt->getSize());

        if (enqueueSuccess) {
            outstandingReads[pkt->getAddr()].push_back(pkt);
            ++nbrOutstandingReads;
        } else {
            retryReq = true;
        }
    } else if (pkt->isWrite()) {
        enqueueSuccess = frontend->receive_external_requests(
            Ramulator::Request::Type::Write,
            pkt->getAddr(),
            0,
            [this](Ramulator::Request& req) {
                auto it = outstandingWrites.find(req.addr);
                panic_if(it == outstandingWrites.end(),
                         "No outstanding Ramulator2 write for %#llx\n",
                         req.addr);

                PacketPtr pkt = it->second.front();
                it->second.pop_front();
                if (it->second.empty()) {
                    outstandingWrites.erase(it);
                }

                --nbrOutstandingWrites;
                accessAndRespond(pkt);
            },
            pkt->getSize());

        if (enqueueSuccess) {
            outstandingWrites[pkt->getAddr()].push_back(pkt);
            ++nbrOutstandingWrites;
        } else {
            retryReq = true;
        }
    }

    return enqueueSuccess;
}

void
Ramulator2::recvRespRetry()
{
    assert(retryResp);
    retryResp = false;
    sendResponse();
}

void
Ramulator2::accessAndRespond(PacketPtr pkt)
{
    const bool needsResponse = pkt->needsResponse();

    access(pkt);

    if (needsResponse) {
        assert(pkt->isResponse());

        Tick responseTick = curTick() + pkt->headerDelay +
            pkt->payloadDelay;
        pkt->headerDelay = pkt->payloadDelay = 0;

        responseQueue.push_back(pkt);

        if (!retryResp && !sendResponseEvent.scheduled()) {
            schedule(sendResponseEvent, responseTick);
        }
    } else {
        pendingDelete.reset(pkt);
    }
}

Port&
Ramulator2::getPort(const std::string& if_name, PortID idx)
{
    if (if_name == "port") {
        return port;
    }

    return ClockedObject::getPort(if_name, idx);
}

DrainState
Ramulator2::drain()
{
    return nbrOutstanding() != 0 ?
        DrainState::Draining : DrainState::Drained;
}

Ramulator2::MemoryPort::MemoryPort(
        const std::string& name, Ramulator2& memory) :
    ResponsePort(name),
    mem(memory)
{
}

Tick
Ramulator2::MemoryPort::recvAtomic(PacketPtr pkt)
{
    return mem.recvAtomic(pkt);
}

void
Ramulator2::MemoryPort::recvFunctional(PacketPtr pkt)
{
    mem.recvFunctional(pkt);
}

bool
Ramulator2::MemoryPort::recvTimingReq(PacketPtr pkt)
{
    return mem.recvTimingReq(pkt);
}

void
Ramulator2::MemoryPort::recvRespRetry()
{
    mem.recvRespRetry();
}

AddrRangeList
Ramulator2::MemoryPort::getAddrRanges() const
{
    return {mem.getAddrRange()};
}

} // namespace memory
} // namespace gem5
