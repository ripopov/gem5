/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/runtime/image_loader.hh"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string_view>

namespace gem5::rtl_cosim
{

namespace
{

bool
range(std::size_t offset, std::size_t size, std::size_t total)
{
    return offset <= total && size <= total - offset;
}

bool
readInteger(const std::vector<std::uint8_t> &bytes, std::size_t offset,
            unsigned width, bool littleEndian, std::uint64_t &value)
{
    if (width > 8 || !range(offset, width, bytes.size())) {
        return false;
    }
    value = 0;
    for (unsigned index = 0; index < width; ++index) {
        const unsigned shift =
            littleEndian ? index * 8 : (width - index - 1) * 8;
        value |= std::uint64_t{bytes[offset + index]} << shift;
    }
    return true;
}

bool
readFile(const std::string &path, std::vector<std::uint8_t> &bytes,
         std::string &error)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        error = "cannot open image '" + path + "'";
        return false;
    }
    const std::streamoff length = stream.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) >
                          std::numeric_limits<std::size_t>::max()) {
        error = "image '" + path + "' is too large";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(length));
    stream.seekg(0);
    if (!bytes.empty() &&
        !stream.read(reinterpret_cast<char *>(bytes.data()), length)) {
        error = "cannot read image '" + path + "'";
        return false;
    }
    return true;
}

bool
loadElf(const std::vector<std::uint8_t> &bytes,
        std::vector<ImageSegment> &segments, std::string &error)
{
    if (bytes.size() < 16 || bytes[0] != 0x7f || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F') {
        error = "not an ELF image";
        return false;
    }
    const bool elf64 = bytes[4] == 2;
    if (!elf64 && bytes[4] != 1) {
        error = "unsupported ELF class";
        return false;
    }
    const bool little = bytes[5] == 1;
    if (!little && bytes[5] != 2) {
        error = "unsupported ELF byte order";
        return false;
    }
    std::uint64_t phOffset = 0;
    std::uint64_t phEntrySize = 0;
    std::uint64_t phCount = 0;
    if (!readInteger(bytes, elf64 ? 32 : 28, elf64 ? 8 : 4, little,
                     phOffset) ||
        !readInteger(bytes, elf64 ? 54 : 42, 2, little, phEntrySize) ||
        !readInteger(bytes, elf64 ? 56 : 44, 2, little, phCount)) {
        error = "truncated ELF header";
        return false;
    }
    const std::uint64_t minimum = elf64 ? 56 : 32;
    if (phEntrySize < minimum || phCount > 65535 || phOffset > bytes.size() ||
        phCount > (bytes.size() - phOffset) / phEntrySize) {
        error = "invalid ELF program-header table";
        return false;
    }
    for (std::uint64_t index = 0; index < phCount; ++index) {
        const std::size_t offset =
            static_cast<std::size_t>(phOffset + index * phEntrySize);
        std::uint64_t type = 0;
        std::uint64_t fileOffset = 0;
        std::uint64_t virtualAddress = 0;
        std::uint64_t physicalAddress = 0;
        std::uint64_t fileSize = 0;
        std::uint64_t memorySize = 0;
        if (!readInteger(bytes, offset, 4, little, type)) {
            error = "truncated ELF program header";
            return false;
        }
        if (type != 1) {
            continue;
        }
        if (elf64) {
            if (!readInteger(bytes, offset + 8, 8, little, fileOffset) ||
                !readInteger(bytes, offset + 16, 8, little, virtualAddress) ||
                !readInteger(bytes, offset + 24, 8, little, physicalAddress) ||
                !readInteger(bytes, offset + 32, 8, little, fileSize) ||
                !readInteger(bytes, offset + 40, 8, little, memorySize)) {
                error = "truncated ELF64 program header";
                return false;
            }
        } else {
            if (!readInteger(bytes, offset + 4, 4, little, fileOffset) ||
                !readInteger(bytes, offset + 8, 4, little, virtualAddress) ||
                !readInteger(bytes, offset + 12, 4, little, physicalAddress) ||
                !readInteger(bytes, offset + 16, 4, little, fileSize) ||
                !readInteger(bytes, offset + 20, 4, little, memorySize)) {
                error = "truncated ELF32 program header";
                return false;
            }
        }
        if (fileSize > memorySize || fileOffset > bytes.size() ||
            fileSize > bytes.size() - fileOffset ||
            memorySize > std::numeric_limits<std::size_t>::max()) {
            error = "invalid ELF load segment";
            return false;
        }
        ImageSegment segment;
        segment.address = physicalAddress ? physicalAddress : virtualAddress;
        segment.memorySize = memorySize;
        segment.data.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(fileOffset),
            bytes.begin() +
                static_cast<std::ptrdiff_t>(fileOffset + fileSize));
        segments.push_back(std::move(segment));
    }
    if (segments.empty()) {
        error = "ELF image contains no loadable segments";
        return false;
    }
    return true;
}

bool
loadCoff(const std::vector<std::uint8_t> &bytes,
         std::vector<ImageSegment> &segments, std::string &error)
{
    bool pe = bytes.size() >= 64 && bytes[0] == 'M' && bytes[1] == 'Z';
    std::uint64_t headerOffset = 0;
    if (pe) {
        if (!readInteger(bytes, 0x3c, 4, true, headerOffset) ||
            !range(static_cast<std::size_t>(headerOffset), 24, bytes.size()) ||
            bytes[headerOffset] != 'P' || bytes[headerOffset + 1] != 'E' ||
            bytes[headerOffset + 2] != 0 || bytes[headerOffset + 3] != 0) {
            error = "invalid PE/COFF header";
            return false;
        }
        headerOffset += 4;
    }
    if (!range(static_cast<std::size_t>(headerOffset), 20, bytes.size())) {
        error = "truncated COFF header";
        return false;
    }
    std::uint64_t sectionCount = 0;
    std::uint64_t optionalSize = 0;
    if (!readInteger(bytes, headerOffset + 2, 2, true, sectionCount) ||
        !readInteger(bytes, headerOffset + 16, 2, true, optionalSize) ||
        sectionCount == 0 || sectionCount > 4096) {
        error = "invalid COFF section count";
        return false;
    }
    std::uint64_t imageBase = 0;
    if (pe && optionalSize >= 32) {
        std::uint64_t magic = 0;
        if (!readInteger(bytes, headerOffset + 20, 2, true, magic)) {
            error = "truncated PE optional header";
            return false;
        }
        const unsigned baseWidth = magic == 0x20b ? 8 : 4;
        const std::size_t baseOffset = magic == 0x20b ? 24 : 28;
        if (!readInteger(bytes, headerOffset + 20 + baseOffset, baseWidth,
                         true, imageBase)) {
            error = "truncated PE image base";
            return false;
        }
    }
    const std::uint64_t sectionsOffset = headerOffset + 20 + optionalSize;
    if (sectionsOffset > bytes.size() ||
        sectionCount > (bytes.size() - sectionsOffset) / 40) {
        error = "truncated COFF section table";
        return false;
    }
    for (std::uint64_t index = 0; index < sectionCount; ++index) {
        const std::size_t offset =
            static_cast<std::size_t>(sectionsOffset + index * 40);
        std::uint64_t virtualSize = 0;
        std::uint64_t virtualAddress = 0;
        std::uint64_t rawSize = 0;
        std::uint64_t rawOffset = 0;
        if (!readInteger(bytes, offset + 8, 4, true, virtualSize) ||
            !readInteger(bytes, offset + 12, 4, true, virtualAddress) ||
            !readInteger(bytes, offset + 16, 4, true, rawSize) ||
            !readInteger(bytes, offset + 20, 4, true, rawOffset)) {
            error = "truncated COFF section header";
            return false;
        }
        const std::uint64_t memorySize =
            pe ? std::max(virtualSize, rawSize) : rawSize;
        if (memorySize == 0) {
            continue;
        }
        if (rawSize != 0 &&
            (rawOffset > bytes.size() || rawSize > bytes.size() - rawOffset)) {
            error = "invalid COFF section data range";
            return false;
        }
        ImageSegment segment;
        segment.address = imageBase + virtualAddress;
        segment.memorySize = memorySize;
        if (rawSize) {
            segment.data.assign(
                bytes.begin() + static_cast<std::ptrdiff_t>(rawOffset),
                bytes.begin() +
                    static_cast<std::ptrdiff_t>(rawOffset + rawSize));
        }
        segments.push_back(std::move(segment));
    }
    if (segments.empty()) {
        error = "COFF image contains no loadable sections";
        return false;
    }
    return true;
}

} // anonymous namespace

bool
loadImage(const std::string &path, const std::string &format,
          std::uint64_t rawAddress, std::vector<ImageSegment> &segments,
          std::string &error)
{
    std::vector<std::uint8_t> bytes;
    if (!readFile(path, bytes, error)) {
        return false;
    }
    segments.clear();
    std::string selected = format;
    if (selected == "auto") {
        if (bytes.size() >= 4 && bytes[0] == 0x7f && bytes[1] == 'E' &&
            bytes[2] == 'L' && bytes[3] == 'F') {
            selected = "elf";
        } else if (bytes.size() >= 2 && bytes[0] == 'M' && bytes[1] == 'Z') {
            selected = "coff";
        } else {
            selected = "raw";
        }
    }
    if (selected == "elf") {
        return loadElf(bytes, segments, error);
    }
    if (selected == "coff") {
        return loadCoff(bytes, segments, error);
    }
    if (selected != "raw") {
        error = "unknown image format '" + format + "'";
        return false;
    }
    segments.push_back({rawAddress, std::move(bytes), 0});
    segments.back().memorySize = segments.back().data.size();
    return true;
}

} // namespace gem5::rtl_cosim
