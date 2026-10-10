/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
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

#include "rt_accel/accelerator.hh"

#include <deque>
#include <utility>
#include <vector>

#include "base/logging.hh"

namespace gem5
{
namespace rt_accel
{
namespace
{

class MockAccelerator final : public Accelerator
{
  private:
    struct Pending
    {
        Tick arrival;
        PacketPtr pkt;
        Completion done;
    };

    const Config config;
    std::vector<uint8_t> registers;
    std::deque<Pending> pending;
    PortAPI *bus = nullptr;
    bool configured = false;
    bool started = false;
    Tick lastEdge = 0;

  public:
    explicit MockAccelerator(const Config &cfg) : config(cfg)
    {
        panic_if(!config.mmioRange.valid() || config.mmioRange.interleaved(),
                 "RT accelerator requires a non-interleaved MMIO range");
        panic_if(!config.clockPeriod || !config.queueDepth,
                 "RT accelerator requires a clock and a nonempty queue");
        registers.resize(config.mmioRange.size(), 0);
    }

    void
    connectNonCoherentBus(PortAPI &responder) override
    {
        panic_if(started || !pending.empty(), "Cannot reconnect active model");
        bus = &responder;
    }

    void
    useExternalTransport(LinkTiming mmio, LinkTiming master) override
    {
        panic_if(started || !pending.empty(), "Cannot retime active model");
        // This mock has no internal transport pipeline. The wrapper owns
        // these delays; real hardware stages must be defined by its model.
        panic_if(!mmio.requestDelay || !mmio.responseDelay ||
                     !master.requestDelay || !master.responseDelay,
                 "External transport requires positive delays");
        configured = true;
    }

    bool
    trySubmit(Tick arrival, PacketPtr pkt, Completion done) override
    {
        panic_if(!configured || !bus, "RT accelerator is not configured");
        panic_if(!pkt || !done || !pkt->isRequest() || !pkt->needsResponse(),
                 "RT accelerator requires a request and a completion");
        panic_if(started && arrival < lastEdge, "Input arrived in the past");
        if (pending.size() == config.queueDepth) {
            return false;
        }
        pending.push_back({arrival, pkt, std::move(done)});
        return true;
    }

    void
    runCycle(Tick edge) override
    {
        panic_if(!configured || !bus, "RT accelerator is not configured");
        panic_if(started && (edge <= lastEdge ||
                             edge - lastEdge != config.clockPeriod),
                 "RT accelerator must advance exactly one cycle at a time");
        started = true;
        lastEdge = edge;

        // A request completes no earlier than one full clock after arrival.
        // Process a snapshot so reentrant submissions wait for another edge.
        const auto count = pending.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (edge < pending.front().arrival ||
                edge - pending.front().arrival < config.clockPeriod) {
                break;
            }
            auto request = std::move(pending.front());
            pending.pop_front();
            PacketPtr pkt = request.pkt;
            const bool read = pkt->cmd == MemCmd::ReadReq;
            const bool write = pkt->cmd == MemCmd::WriteReq;
            const Addr addr = pkt->getAddr();
            const bool valid = config.mmioRange.contains(addr) &&
                               pkt->getSize() <= config.mmioRange.end() - addr;
            if (valid && write) {
                pkt->writeData(registers.data() + addr -
                               config.mmioRange.start());
            }
            pkt->makeResponse();
            if (!valid) {
                pkt->setBadAddress();
            } else if (!read && !write) {
                pkt->setBadCommand();
            } else if (read) {
                pkt->setData(registers.data() + addr -
                             config.mmioRange.start());
            }
            // The callback may delete pkt. Do not access it afterward.
            request.done(edge, pkt);
        }
    }

    bool
    idle() const override
    {
        return pending.empty();
    }
};

} // anonymous namespace

std::unique_ptr<Accelerator>
createAccelerator(const Config &config)
{
    return std::make_unique<MockAccelerator>(config);
}

} // namespace rt_accel
} // namespace gem5
