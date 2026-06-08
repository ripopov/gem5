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

TEST_F(FstTraceRoundTripTest, MessageBufferSignalsRoundTrip)
{
    fstWriterContext *writer = fstWriterCreate(fstPath.c_str(), 1);
    ASSERT_NE(writer, nullptr);

    fstWriterSetTimescale(writer, -12);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "system", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "ruby", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "hnf00", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "cntrl", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "reqIn", nullptr);

    fstHandle push = fstWriterCreateVar(writer, FST_VT_VCD_EVENT,
                                        FST_VD_IMPLICIT, 1, "push", 0);
    fstHandle pop = fstWriterCreateVar(writer, FST_VT_VCD_EVENT,
                                       FST_VD_IMPLICIT, 1, "pop", 0);
    fstHandle current_size =
        fstWriterCreateVar(writer, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                           "current_size", 0);
    ASSERT_NE(push, 0u);
    ASSERT_NE(pop, 0u);
    ASSERT_NE(current_size, 0u);

    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitValueChange(writer, push, "1");
    fstWriterEmitValueChange64(writer, current_size, 32, 1);
    fstWriterEmitTimeChange(writer, 20);
    fstWriterEmitValueChange(writer, pop, "1");
    fstWriterEmitValueChange64(writer, current_size, 32, 0);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen(fstPath.c_str());
    ASSERT_NE(reader, nullptr);

    std::vector<unsigned char> var_types;
    std::vector<std::string> var_names;
    while (auto *hier = fstReaderIterateHier(reader)) {
        if (hier->htyp == FST_HT_VAR) {
            var_types.push_back(hier->u.var.typ);
            var_names.emplace_back(hier->u.var.name);
        }
    }

    EXPECT_EQ(var_names,
              (std::vector<std::string>{"push", "pop", "current_size"}));
    ASSERT_EQ(var_types.size(), 3u);
    EXPECT_EQ(var_types[0], FST_VT_VCD_EVENT);
    EXPECT_EQ(var_types[1], FST_VT_VCD_EVENT);
    EXPECT_EQ(var_types[2], FST_VT_VCD_INTEGER);

    fstReaderSetFacProcessMaskAll(reader);
    std::vector<uint64_t> change_times;
    fstReaderIterBlocks(
        reader,
        [](void *data, uint64_t time, fstHandle, const unsigned char *) {
            auto *times = static_cast<std::vector<uint64_t> *>(data);
            times->push_back(time);
        },
        &change_times, nullptr);

    EXPECT_EQ(change_times, (std::vector<uint64_t>{10, 10, 20, 20}));
    fstReaderClose(reader);
}

TEST_F(FstTraceRoundTripTest, AliasedVariablesRoundTrip)
{
    fstWriterContext *writer = fstWriterCreate(fstPath.c_str(), 1);
    ASSERT_NE(writer, nullptr);

    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "system", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "ruby", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "network", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "int_links000", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "buffers0", nullptr);
    fstHandle original = fstWriterCreateVar(
        writer, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32, "current_size", 0);
    ASSERT_NE(original, 0u);
    fstWriterSetUpscope(writer);
    fstWriterSetUpscope(writer);

    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "routers00", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE,
                      "out_East_to_routers01_int_links000", nullptr);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "buffers0", nullptr);
    fstHandle alias =
        fstWriterCreateVar(writer, FST_VT_VCD_INTEGER, FST_VD_IMPLICIT, 32,
                           "current_size", original);
    ASSERT_EQ(alias, original);
    fstWriterSetUpscope(writer);
    fstWriterSetUpscope(writer);
    fstWriterSetUpscope(writer);
    fstWriterSetUpscope(writer);
    fstWriterSetUpscope(writer);
    fstWriterSetUpscope(writer);

    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitValueChange64(writer, original, 32, 7);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen(fstPath.c_str());
    ASSERT_NE(reader, nullptr);

    std::vector<std::string> vars;
    std::vector<fstHandle> handles;
    while (auto *hier = fstReaderIterateHier(reader)) {
        if (hier->htyp == FST_HT_VAR) {
            vars.emplace_back(hier->u.var.name);
            handles.push_back(hier->u.var.handle);
        }
    }

    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars,
              (std::vector<std::string>{"current_size", "current_size"}));
    ASSERT_EQ(handles.size(), 2u);
    EXPECT_EQ(handles[0], handles[1]);

    fstReaderClose(reader);
}

} // anonymous namespace
