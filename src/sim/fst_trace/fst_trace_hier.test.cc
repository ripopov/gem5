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

#include <cstdio>
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

} // anonymous namespace
