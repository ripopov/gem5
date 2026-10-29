/*
 * Copyright (c) 2014-2015 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "gpu-compute/lds_state.hh"

#include <algorithm>

#include "gpu-compute/compute_unit.hh"
#include "gpu-compute/gpu_dyn_inst.hh"
#include "gpu-compute/shader.hh"

namespace gem5
{

/**
 * the default constructor that works with SWIG
 */
LdsState::LdsState(const Params &params)
    : ClockedObject(params),
      tickEvent(this),
      cuPort(name() + ".port", this),
      maximumSize(params.size),
      range(params.range),
      bankConflictPenalty(params.bankConflictPenalty),
      banks(params.banks)
{
    fatal_if(params.banks <= 0,
             "Number of LDS banks should be positive number");
    fatal_if((params.banks & (params.banks - 1)) != 0,
             "Number of LDS banks should be a power of 2");
    fatal_if(params.size <= 0,
             "cannot allocate an LDS with a size less than 1");
    fatal_if(params.size % 2, "the LDS should be an even number");
}

/**
 * set the parent and name based on the parent
 */
void
LdsState::setParent(ComputeUnit *x_parent)
{
    // check that this gets assigned to the same thing each time
    fatal_if(!x_parent, "x_parent should not be nullptr");
    fatal_if(x_parent == parent, "should not be setting the parent twice");

    parent = x_parent;
    _name = x_parent->name() + ".LdsState";
}

/**
 * receive the packet from the CU
 */
bool
LdsState::CuSidePort::recvTimingReq(PacketPtr packet)
{
    return ownerLds->processPacket(packet);
}

GPUDynInstPtr
LdsState::getDynInstr(PacketPtr packet)
{
    ComputeUnit::LDSPort::SenderState *ss =
        dynamic_cast<ComputeUnit::LDSPort::SenderState *>(packet->senderState);
    return ss->getMemInst();
}

/**
 * process an incoming packet, add it to the return queue
 */
bool
LdsState::processPacket(PacketPtr packet)
{
    GPUDynInstPtr dynInst = getDynInstr(packet);
    const auto banksUsed = countLdsBankAccesses(
        dynInst->addr, dynInst->exec_mask.to_ullong(), dynInst->ldsAccess,
        banks, dynInst->isLoad() && !dynInst->isAtomic());
    parent->stats.ldsBankAccesses += banksUsed.accesses;
    parent->stats.ldsBankConflictDist.sample(banksUsed.conflicts);

    const unsigned busLength = dynInst->isStore() ? parent->storeBusLength()
                                                  : parent->loadBusLength();
    // Bank service and the transfer impose baseline throughput bounds.
    // Only additional serialization pays the configurable conflict penalty;
    // the instruction's existing latency remains the completion lower bound.
    const unsigned cycles = std::max(busLength, banksUsed.phases) +
                            banksUsed.conflicts * bankConflictPenalty;
    Tick processingTime = parent->cyclesToTicks(Cycles(cycles));
    // choose (delay + last packet in queue) or (now + delay) as the time to
    // return this
    Tick doneAt = earliestReturnTime() + processingTime;
    // then store it for processing
    return returnQueuePush(std::make_pair(doneAt, packet));
}

/**
 * add this to the queue of packets to be returned
 */
bool
LdsState::returnQueuePush(std::pair<Tick, PacketPtr> thePair)
{
    // TODO add time limits (e.g. one packet per cycle) and queue size limits
    // and implement flow control
    returnQueue.push(thePair);

    // if there is no set wakeup time, look through the queue
    if (!tickEvent.scheduled()) {
        process();
    }

    return true;
}

/**
 * receive a packet in functional mode
 */
void
LdsState::CuSidePort::recvFunctional(PacketPtr pkt)
{
    fatal("not implemented");
}

/**
 * receive a retry for a response
 */
void
LdsState::CuSidePort::recvRespRetry()
{
    // TODO verify that this is the right way to do this
    assert(ownerLds->isRetryResp());
    ownerLds->setRetryResp(false);
    ownerLds->process();
}

/**
 * receive a retry
 */
void
LdsState::CuSidePort::recvRetry()
{
    fatal("not implemented");
}

/**
 * look for packets to return at this time
 */
bool
LdsState::process()
{
    Tick now = clockEdge();

    // send back completed packets
    while (!returnQueue.empty() && returnQueue.front().first <= now) {
        PacketPtr packet = returnQueue.front().second;

        ComputeUnit::LDSPort::SenderState *ss =
            dynamic_cast<ComputeUnit::LDSPort::SenderState *>(
                packet->senderState);

        GPUDynInstPtr gpuDynInst = ss->getMemInst();

        gpuDynInst->initiateAcc(gpuDynInst);

        packet->makeTimingResponse();

        returnQueue.pop();

        bool success = cuPort.sendTimingResp(packet);

        if (!success) {
            retryResp = true;
            panic("have not handled timing responses being NACK'd when sent"
                  "back");
        }
    }

    // determine the next wakeup time
    if (!returnQueue.empty()) {

        Tick next = returnQueue.front().first;

        if (tickEvent.scheduled()) {

            if (next < tickEvent.when()) {

                tickEvent.deschedule();
                tickEvent.schedule(next);
            }
        } else {
            tickEvent.schedule(next);
        }
    }

    return true;
}

/**
 * wake up at this time and perform specified actions
 */
void
LdsState::TickEvent::process()
{
    ldsState->process();
}

} // namespace gem5
