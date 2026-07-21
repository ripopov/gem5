/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "rtl/runtime/image_loader.hh"

namespace gem5::rtl_cosim
{
namespace
{

class TemporaryImage
{
  public:
    TemporaryImage(std::string name, const std::vector<std::uint8_t> &data)
        : path(std::filesystem::temp_directory_path() / std::move(name))
    {
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char *>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    }
    ~TemporaryImage() { std::filesystem::remove(path); }
    std::filesystem::path path;
};

void
put16(std::vector<std::uint8_t> &data, std::size_t at, std::uint16_t value)
{
    data[at] = static_cast<std::uint8_t>(value);
    data[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void
put32(std::vector<std::uint8_t> &data, std::size_t at, std::uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index) {
        data[at + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

TEST(ImageLoader, LoadsRawAtConfiguredAddress)
{
    TemporaryImage image("rtl-cosim-raw.bin", {1, 2, 3, 4});
    std::vector<ImageSegment> segments;
    std::string error;
    ASSERT_TRUE(loadImage(image.path.string(), "raw", 0x100, segments, error))
        << error;
    ASSERT_EQ(segments.size(), 1);
    EXPECT_EQ(segments[0].address, 0x100);
    EXPECT_EQ(segments[0].data, (std::vector<std::uint8_t>{1, 2, 3, 4}));
}

TEST(ImageLoader, LoadsElf32SegmentAndBssSize)
{
    std::vector<std::uint8_t> elf(0x104, 0);
    elf[0] = 0x7f;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 1;
    elf[5] = 1;
    elf[6] = 1;
    put32(elf, 28, 52);
    put16(elf, 42, 32);
    put16(elf, 44, 1);
    put32(elf, 52, 1);
    put32(elf, 56, 0x100);
    put32(elf, 60, 0x8000);
    put32(elf, 64, 0x8000);
    put32(elf, 68, 4);
    put32(elf, 72, 16);
    elf[0x100] = 0xde;
    elf[0x101] = 0xad;
    elf[0x102] = 0xbe;
    elf[0x103] = 0xef;
    TemporaryImage image("rtl-cosim-test.elf", elf);
    std::vector<ImageSegment> segments;
    std::string error;
    ASSERT_TRUE(loadImage(image.path.string(), "auto", 0, segments, error))
        << error;
    ASSERT_EQ(segments.size(), 1);
    EXPECT_EQ(segments[0].address, 0x8000);
    EXPECT_EQ(segments[0].memorySize, 16);
    EXPECT_EQ(segments[0].data,
              (std::vector<std::uint8_t>{0xde, 0xad, 0xbe, 0xef}));
}

TEST(ImageLoader, RejectsTruncatedElf)
{
    TemporaryImage image("rtl-cosim-bad.elf", {0x7f, 'E', 'L', 'F'});
    std::vector<ImageSegment> segments;
    std::string error;
    EXPECT_FALSE(loadImage(image.path.string(), "elf", 0, segments, error));
    EXPECT_FALSE(error.empty());
}

TEST(ImageLoader, LoadsCoffSection)
{
    std::vector<std::uint8_t> coff(64, 0);
    put16(coff, 2, 1);
    put16(coff, 16, 0);
    put32(coff, 20 + 12, 0x9000);
    put32(coff, 20 + 16, 4);
    put32(coff, 20 + 20, 60);
    coff[60] = 0xca;
    coff[61] = 0xfe;
    coff[62] = 0xba;
    coff[63] = 0xbe;
    TemporaryImage image("rtl-cosim-test.coff", coff);
    std::vector<ImageSegment> segments;
    std::string error;
    ASSERT_TRUE(loadImage(image.path.string(), "coff", 0, segments, error))
        << error;
    ASSERT_EQ(segments.size(), 1);
    EXPECT_EQ(segments[0].address, 0x9000);
    EXPECT_EQ(segments[0].memorySize, 4);
    EXPECT_EQ(segments[0].data,
              (std::vector<std::uint8_t>{0xca, 0xfe, 0xba, 0xbe}));
}

} // anonymous namespace
} // namespace gem5::rtl_cosim
