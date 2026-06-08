/*
 * Copyright (c) 2026 The Regents of The University of California
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

#ifndef __SIM_FST_TRACE_FST_TRACE_HH__
#define __SIM_FST_TRACE_FST_TRACE_HH__

#include <fstapi.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "params/FstTrace.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace ruby
{
class MessageBuffer;
} // namespace ruby

class FstTrace : public SimObject
{
  public:
    PARAMS(FstTrace);

    explicit FstTrace(const Params &p);
    ~FstTrace() override;

    void init() override;
    void startup() override;

    static void recordMessageBufferPush(const ruby::MessageBuffer *buffer,
                                        Tick tick);
    static void recordMessageBufferPop(const ruby::MessageBuffer *buffer,
                                       Tick tick);

  private:
    fstWriterContext *fstCtx = nullptr;

    std::vector<const SimObject *> simObjects;
    std::mutex writerMutex;

    std::string resolvedTracePath;
    Tick lastWrittenTick = 0;
    bool hasWrittenTime = false;

    void closeTrace();
    void collectMessageBuffersAndAliases();
    void emitHierarchy();
    void emitTimeChangeLocked(Tick tick);

    struct MessageBufferSignals
    {
        fstHandle push = 0;
        fstHandle pop = 0;
        fstHandle currentSize = 0;
        fstHandle occupiedSlots = 0;
        fstHandle stalledMessages = 0;
        fstHandle deferredMessages = 0;
        fstHandle capacity = 0;
        fstHandle unbounded = 0;
        fstHandle maxDequeueRate = 0;
        fstHandle totalEnqueued = 0;
        fstHandle totalDequeued = 0;
        fstHandle notAvailableCount = 0;
        fstHandle stallCount = 0;
        fstHandle stallTicks = 0;
        fstHandle bufferedMessagesStat = 0;
        fstHandle dequeuesThisCycle = 0;
        fstHandle vnet = 0;
        fstHandle incomingLink = 0;
        fstHandle routingPriority = 0;
        fstHandle strictFifo = 0;
        fstHandle allowZeroLatency = 0;
        fstHandle randomization = 0;
        fstHandle headReadyTick = 0;
    };

    struct MessageBufferAlias
    {
        std::string linkScope;
        std::string bufferScope;
        const ruby::MessageBuffer *buffer = nullptr;
    };

    std::vector<const ruby::MessageBuffer *> messageBuffers;
    std::unordered_map<const ruby::MessageBuffer *, MessageBufferSignals>
        messageBufferSignalMap;
    std::unordered_map<std::string, std::vector<MessageBufferAlias>>
        messageBufferAliasesByRouter;

    MessageBufferSignals
    createMessageBufferSignals(const ruby::MessageBuffer *buffer);
    void createMessageBufferAliasSignals(const MessageBufferSignals &target);
    void emitMessageBufferAliasesForScope(const std::string &scope);
    void emitInitialMessageBufferStates();
    void emitMessageBufferStateLocked(const ruby::MessageBuffer *buffer,
                                      const MessageBufferSignals &signals);
    void recordMessageBufferEvent(const ruby::MessageBuffer *buffer, Tick tick,
                                  bool is_push);
};

} // namespace gem5

#endif // __SIM_FST_TRACE_FST_TRACE_HH__
