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

#include <cstdio>
#include <string>
#include <vector>

class FstTest : public ::testing::Test
{
  protected:
    std::string fstPath;

    void
    SetUp() override
    {
        // Create a unique temp file path for each test
        char tmpl[] = "/tmp/gem5_fst_test_XXXXXX";
        int fd = mkstemp(tmpl);
        ASSERT_NE(fd, -1);
        close(fd);
        fstPath = std::string(tmpl) + ".fst";
        // Remove the temp file created by mkstemp; fstapi will create .fst
        unlink(tmpl);
    }

    void
    TearDown() override
    {
        unlink(fstPath.c_str());
    }
};

// Test that we can create and close an empty FST file
TEST_F(FstTest, CreateEmptyFile)
{
    fstWriterContext *writer = fstWriterCreate(fstPath.c_str(), 1);
    ASSERT_NE(writer, nullptr);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen(fstPath.c_str());
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(fstReaderGetVarCount(reader), 0u);
    fstReaderClose(reader);
}

// Test writing a single 1-bit signal and reading it back
TEST_F(FstTest, WriteSingleBitSignal)
{
    // Write
    fstWriterContext *writer = fstWriterCreate(fstPath.c_str(), 1);
    ASSERT_NE(writer, nullptr);

    fstWriterSetTimescale(writer, -9); // 1 ns

    fstHandle clk = fstWriterCreateVar(writer, FST_VT_VCD_WIRE,
                                       FST_VD_IMPLICIT, 1, "clk", 0);
    EXPECT_NE(clk, 0u);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, clk, "0");
    fstWriterEmitTimeChange(writer, 5);
    fstWriterEmitValueChange(writer, clk, "1");
    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitValueChange(writer, clk, "0");
    fstWriterEmitTimeChange(writer, 15);
    fstWriterEmitValueChange(writer, clk, "1");
    fstWriterEmitTimeChange(writer, 20);

    fstWriterClose(writer);

    // Read back metadata
    fstReaderContext *reader = fstReaderOpen(fstPath.c_str());
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(fstReaderGetStartTime(reader), 0u);
    EXPECT_EQ(fstReaderGetEndTime(reader), 20u);
    EXPECT_EQ(fstReaderGetVarCount(reader), 1u);
    EXPECT_EQ(fstReaderGetTimescale(reader), -9);

    // Iterate hierarchy: expect one var
    struct fstHier *hier = fstReaderIterateHier(reader);
    ASSERT_NE(hier, nullptr);
    EXPECT_EQ(hier->htyp, FST_HT_VAR);
    EXPECT_STREQ(hier->u.var.name, "clk");
    EXPECT_EQ(hier->u.var.length, 1u);

    // No more hierarchy entries
    hier = fstReaderIterateHier(reader);
    EXPECT_EQ(hier, nullptr);

    fstReaderClose(reader);
}

// Test writing a multi-bit bus and reading back hierarchy
TEST_F(FstTest, WriteMultiBitBus)
{
    fstWriterContext *writer = fstWriterCreate(fstPath.c_str(), 1);
    ASSERT_NE(writer, nullptr);

    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", NULL);

    fstHandle data = fstWriterCreateVar(writer, FST_VT_VCD_WIRE,
                                        FST_VD_IMPLICIT, 8, "data", 0);
    EXPECT_NE(data, 0u);

    fstWriterSetUpscope(writer);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, data, "00000000");
    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitValueChange(writer, data, "10101010");
    fstWriterEmitTimeChange(writer, 20);
    fstWriterEmitValueChange(writer, data, "11111111");
    fstWriterEmitTimeChange(writer, 30);

    fstWriterClose(writer);

    // Read back
    fstReaderContext *reader = fstReaderOpen(fstPath.c_str());
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(fstReaderGetVarCount(reader), 1u);
    EXPECT_EQ(fstReaderGetScopeCount(reader), 1u);
    EXPECT_EQ(fstReaderGetStartTime(reader), 0u);
    EXPECT_EQ(fstReaderGetEndTime(reader), 30u);

    // Iterate hierarchy: scope -> var -> upscope
    struct fstHier *hier = fstReaderIterateHier(reader);
    ASSERT_NE(hier, nullptr);
    EXPECT_EQ(hier->htyp, FST_HT_SCOPE);
    EXPECT_STREQ(hier->u.scope.name, "top");

    hier = fstReaderIterateHier(reader);
    ASSERT_NE(hier, nullptr);
    EXPECT_EQ(hier->htyp, FST_HT_VAR);
    EXPECT_STREQ(hier->u.var.name, "data");
    EXPECT_EQ(hier->u.var.length, 8u);

    hier = fstReaderIterateHier(reader);
    ASSERT_NE(hier, nullptr);
    EXPECT_EQ(hier->htyp, FST_HT_UPSCOPE);

    hier = fstReaderIterateHier(reader);
    EXPECT_EQ(hier, nullptr);

    fstReaderClose(reader);
}

// Test writing multiple signals with value change callbacks
TEST_F(FstTest, ReadValueChanges)
{
    // Write a file with two signals
    fstWriterContext *writer = fstWriterCreate(fstPath.c_str(), 1);
    ASSERT_NE(writer, nullptr);

    fstHandle sig_a = fstWriterCreateVar(writer, FST_VT_VCD_WIRE,
                                         FST_VD_IMPLICIT, 1, "sig_a", 0);
    fstHandle sig_b = fstWriterCreateVar(writer, FST_VT_VCD_WIRE,
                                         FST_VD_IMPLICIT, 1, "sig_b", 0);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, sig_a, "0");
    fstWriterEmitValueChange(writer, sig_b, "1");
    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitValueChange(writer, sig_a, "1");
    fstWriterEmitTimeChange(writer, 20);
    fstWriterEmitValueChange(writer, sig_b, "0");
    fstWriterEmitTimeChange(writer, 30);

    fstWriterClose(writer);

    // Read with value change iteration
    fstReaderContext *reader = fstReaderOpen(fstPath.c_str());
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(fstReaderGetVarCount(reader), 2u);

    // Select all signals for value change iteration
    fstReaderSetFacProcessMaskAll(reader);

    struct CallbackData
    {
        std::vector<std::pair<uint64_t, std::string>> changes;
    };
    CallbackData cbdata;

    // Use the block-based iteration
    fstReaderIterBlocks(
        reader,
        [](void *user_callback_data_pointer, uint64_t time, fstHandle facidx,
           const unsigned char *value) {
            auto *d = static_cast<CallbackData *>(user_callback_data_pointer);
            d->changes.emplace_back(
                time, std::string(reinterpret_cast<const char *>(value)));
        },
        &cbdata, nullptr);

    // We expect value changes at times 0, 10, 20
    ASSERT_GE(cbdata.changes.size(), 3u);

    fstReaderClose(reader);
}
