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

#include <fstapi.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{

class FstTraceRoundTripTest : public ::testing::Test
{
  protected:
    std::string fstPath;

    void
    SetUp() override
    {
        char tmpl[] = "/tmp/gem5_fst_trace_XXXXXX";
        int fd = mkstemp(tmpl);
        ASSERT_NE(fd, -1);
        close(fd);
        fstPath = std::string(tmpl) + ".fst";
        unlink(tmpl);
    }

    void
    TearDown() override
    {
        unlink(fstPath.c_str());
    }
};

TEST_F(FstTraceRoundTripTest, HierarchyAndEventMarkersRoundTrip)
{
    fstWriterContext *writer = fstWriterCreate(fstPath.c_str(), 1);
    ASSERT_NE(writer, nullptr);

    fstWriterSetTimescale(writer, -12);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "root", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "tester", nullptr);
    fstHandle event =
        fstWriterCreateVar(writer, FST_VT_VCD_EVENT, FST_VD_IMPLICIT, 1,
                           "setStats_wrapped_function_event", 0);
    ASSERT_NE(event, 0u);
    fstWriterSetUpscope(writer);
    fstWriterSetUpscope(writer);

    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitValueChange(writer, event, "1");
    fstWriterEmitTimeChange(writer, 20);
    fstWriterEmitValueChange(writer, event, "1");
    fstWriterEmitTimeChange(writer, 30);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen(fstPath.c_str());
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(fstReaderGetScopeCount(reader), 2u);
    EXPECT_EQ(fstReaderGetVarCount(reader), 1u);
    EXPECT_EQ(fstReaderGetStartTime(reader), 10u);
    EXPECT_EQ(fstReaderGetEndTime(reader), 30u);

    struct fstHier *hier = fstReaderIterateHier(reader);
    ASSERT_NE(hier, nullptr);
    EXPECT_EQ(hier->htyp, FST_HT_SCOPE);
    EXPECT_STREQ(hier->u.scope.name, "root");

    hier = fstReaderIterateHier(reader);
    ASSERT_NE(hier, nullptr);
    EXPECT_EQ(hier->htyp, FST_HT_SCOPE);
    EXPECT_STREQ(hier->u.scope.name, "tester");

    hier = fstReaderIterateHier(reader);
    ASSERT_NE(hier, nullptr);
    EXPECT_EQ(hier->htyp, FST_HT_VAR);
    EXPECT_EQ(hier->u.var.typ, FST_VT_VCD_EVENT);
    EXPECT_STREQ(hier->u.var.name, "setStats_wrapped_function_event");

    fstReaderSetFacProcessMaskAll(reader);
    std::vector<uint64_t> change_times;
    fstReaderIterBlocks(
        reader,
        [](void *data, uint64_t time, fstHandle, const unsigned char *) {
            auto *times = static_cast<std::vector<uint64_t> *>(data);
            times->push_back(time);
        },
        &change_times, nullptr);

    EXPECT_EQ(change_times, (std::vector<uint64_t>{10, 20}));
    fstReaderClose(reader);
}

TEST_F(FstTraceRoundTripTest, BlackoutRegionsRoundTrip)
{
    fstWriterContext *writer = fstWriterCreate(fstPath.c_str(), 1);
    ASSERT_NE(writer, nullptr);

    fstHandle event = fstWriterCreateVar(writer, FST_VT_VCD_EVENT,
                                         FST_VD_IMPLICIT, 1, "event", 0);
    ASSERT_NE(event, 0u);

    fstWriterEmitTimeChange(writer, 5);
    fstWriterEmitValueChange(writer, event, "1");
    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitDumpActive(writer, 0);
    fstWriterEmitTimeChange(writer, 20);
    fstWriterEmitDumpActive(writer, 1);
    fstWriterEmitTimeChange(writer, 25);
    fstWriterEmitValueChange(writer, event, "1");
    fstWriterEmitTimeChange(writer, 30);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen(fstPath.c_str());
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(fstReaderGetNumberDumpActivityChanges(reader), 2u);
    EXPECT_EQ(fstReaderGetDumpActivityChangeTime(reader, 0), 10u);
    EXPECT_EQ(fstReaderGetDumpActivityChangeValue(reader, 0), 0u);
    EXPECT_EQ(fstReaderGetDumpActivityChangeTime(reader, 1), 20u);
    EXPECT_EQ(fstReaderGetDumpActivityChangeValue(reader, 1), 1u);

    fstReaderClose(reader);
}

TEST_F(FstTraceRoundTripTest, RealValuedSignalRoundTrip)
{
    fstWriterContext *writer = fstWriterCreate(fstPath.c_str(), 1);
    ASSERT_NE(writer, nullptr);

    fstWriterSetTimescale(writer, -12);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "stats", nullptr);
    fstHandle sig = fstWriterCreateVar(writer, FST_VT_VCD_REAL, FST_VD_OUTPUT,
                                       64, "numCycles", 0);
    ASSERT_NE(sig, 0u);
    fstWriterSetUpscope(writer);

    double val1 = 42.5;
    double val2 = 1234567.89;
    double val3 = 0.0;

    fstWriterEmitTimeChange(writer, 100);
    fstWriterEmitValueChange(writer, sig, &val1);
    fstWriterEmitTimeChange(writer, 200);
    fstWriterEmitValueChange(writer, sig, &val2);
    fstWriterEmitTimeChange(writer, 300);
    fstWriterEmitValueChange(writer, sig, &val3);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen(fstPath.c_str());
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(fstReaderGetStartTime(reader), 100u);
    EXPECT_EQ(fstReaderGetEndTime(reader), 300u);

    // Verify hierarchy
    struct fstHier *hier = fstReaderIterateHier(reader);
    ASSERT_NE(hier, nullptr);
    EXPECT_EQ(hier->htyp, FST_HT_SCOPE);
    EXPECT_STREQ(hier->u.scope.name, "stats");

    hier = fstReaderIterateHier(reader);
    ASSERT_NE(hier, nullptr);
    EXPECT_EQ(hier->htyp, FST_HT_VAR);
    EXPECT_EQ(hier->u.var.typ, FST_VT_VCD_REAL);
    EXPECT_STREQ(hier->u.var.name, "numCycles");

    // Read back values
    struct RealChange
    {
        uint64_t time;
        double value;
    };
    std::vector<RealChange> changes;

    fstReaderSetFacProcessMaskAll(reader);
    fstReaderIterBlocks(
        reader,
        [](void *data, uint64_t time, fstHandle, const unsigned char *value) {
            auto *out = static_cast<std::vector<RealChange> *>(data);
            // FST reader passes real values as ASCII strings
            double v =
                std::strtod(reinterpret_cast<const char *>(value), nullptr);
            out->push_back({time, v});
        },
        &changes, nullptr);

    ASSERT_EQ(changes.size(), 3u);
    EXPECT_EQ(changes[0].time, 100u);
    EXPECT_DOUBLE_EQ(changes[0].value, 42.5);
    EXPECT_EQ(changes[1].time, 200u);
    EXPECT_DOUBLE_EQ(changes[1].value, 1234567.89);
    EXPECT_EQ(changes[2].time, 300u);
    EXPECT_DOUBLE_EQ(changes[2].value, 0.0);

    fstReaderClose(reader);
}

TEST_F(FstTraceRoundTripTest, RealValuedDeltaOnlyEncoding)
{
    // Verify that emitting the same double value at two different times
    // still produces two value change records (FST stores all emitted
    // changes — delta-only is an optimization in our writer code, not
    // in the FST format itself). This test validates the round-trip
    // behavior that our delta logic depends on.
    fstWriterContext *writer = fstWriterCreate(fstPath.c_str(), 1);
    ASSERT_NE(writer, nullptr);

    fstHandle sig = fstWriterCreateVar(writer, FST_VT_VCD_REAL, FST_VD_OUTPUT,
                                       64, "counter", 0);
    ASSERT_NE(sig, 0u);

    double val = 100.0;
    fstWriterEmitTimeChange(writer, 1000);
    fstWriterEmitValueChange(writer, sig, &val);
    fstWriterEmitTimeChange(writer, 2000);
    fstWriterEmitValueChange(writer, sig, &val); // same value
    fstWriterEmitTimeChange(writer, 3000);
    double new_val = 200.0;
    fstWriterEmitValueChange(writer, sig, &new_val); // changed
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen(fstPath.c_str());
    ASSERT_NE(reader, nullptr);

    struct RealChange
    {
        uint64_t time;
        double value;
    };
    std::vector<RealChange> changes;

    fstReaderSetFacProcessMaskAll(reader);
    fstReaderIterBlocks(
        reader,
        [](void *data, uint64_t time, fstHandle, const unsigned char *value) {
            auto *out = static_cast<std::vector<RealChange> *>(data);
            double v =
                std::strtod(reinterpret_cast<const char *>(value), nullptr);
            out->push_back({time, v});
        },
        &changes, nullptr);

    // FST stores all explicitly emitted changes
    ASSERT_EQ(changes.size(), 3u);
    EXPECT_DOUBLE_EQ(changes[0].value, 100.0);
    EXPECT_DOUBLE_EQ(changes[1].value, 100.0);
    EXPECT_DOUBLE_EQ(changes[2].value, 200.0);

    fstReaderClose(reader);
}

TEST_F(FstTraceRoundTripTest, StatScopeHierarchyRoundTrip)
{
    fstWriterContext *writer = fstWriterCreate(fstPath.c_str(), 1);
    ASSERT_NE(writer, nullptr);

    fstWriterSetTimescale(writer, -12);

    // Build a hierarchy like the stat sampling would produce:
    // stats / system / cpu0 / {numCycles, numInsts}
    // stats / system / mem_ctrl / {bytesRead, bytesRead_mean}
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "stats", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "system", nullptr);

    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "cpu0", nullptr);
    fstHandle numCycles = fstWriterCreateVar(
        writer, FST_VT_VCD_REAL, FST_VD_OUTPUT, 64, "numCycles", 0);
    fstHandle numInsts = fstWriterCreateVar(writer, FST_VT_VCD_REAL,
                                            FST_VD_OUTPUT, 64, "numInsts", 0);
    ASSERT_NE(numCycles, 0u);
    ASSERT_NE(numInsts, 0u);
    fstWriterSetUpscope(writer); // cpu0

    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "mem_ctrl", nullptr);
    fstHandle bytesRead = fstWriterCreateVar(
        writer, FST_VT_VCD_REAL, FST_VD_OUTPUT, 64, "bytesRead", 0);
    fstHandle bytesReadMean = fstWriterCreateVar(
        writer, FST_VT_VCD_REAL, FST_VD_OUTPUT, 64, "bytesRead_mean", 0);
    ASSERT_NE(bytesRead, 0u);
    ASSERT_NE(bytesReadMean, 0u);
    fstWriterSetUpscope(writer); // mem_ctrl

    fstWriterSetUpscope(writer); // system
    fstWriterSetUpscope(writer); // stats

    // Emit some values
    double cycles = 1000.0;
    double insts = 500.0;
    double bytes = 4096.0;
    double mean = 64.0;

    fstWriterEmitTimeChange(writer, 50000);
    fstWriterEmitValueChange(writer, numCycles, &cycles);
    fstWriterEmitValueChange(writer, numInsts, &insts);
    fstWriterEmitValueChange(writer, bytesRead, &bytes);
    fstWriterEmitValueChange(writer, bytesReadMean, &mean);
    fstWriterClose(writer);

    // Read back and verify hierarchy
    fstReaderContext *reader = fstReaderOpen(fstPath.c_str());
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(fstReaderGetScopeCount(reader),
              4u); // stats, system, cpu0, mem_ctrl
    EXPECT_EQ(fstReaderGetVarCount(reader), 4u);

    std::vector<std::string> scope_names;
    std::vector<std::string> var_names;

    while (auto *hier = fstReaderIterateHier(reader)) {
        if (hier->htyp == FST_HT_SCOPE) {
            scope_names.emplace_back(hier->u.scope.name);
        } else if (hier->htyp == FST_HT_VAR) {
            var_names.emplace_back(hier->u.var.name);
        }
    }

    EXPECT_EQ(scope_names, (std::vector<std::string>{"stats", "system", "cpu0",
                                                     "mem_ctrl"}));
    EXPECT_EQ(var_names,
              (std::vector<std::string>{"numCycles", "numInsts", "bytesRead",
                                        "bytesRead_mean"}));

    fstReaderClose(reader);
}

} // anonymous namespace
