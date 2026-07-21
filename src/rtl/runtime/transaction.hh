/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_COSIM_RUNTIME_TRANSACTION_HH__
#define __RTL_COSIM_RUNTIME_TRANSACTION_HH__

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace gem5::rtl_cosim
{

enum class BurstType : std::uint8_t
{
    Fixed = 0,
    Increment = 1,
    Wrap = 2
};

struct MemoryRequest
{
    std::uint64_t token = 0;
    std::uint32_t id = 0;
    std::uint64_t address = 0;
    bool write = false;
    std::size_t beatBytes = 0;
    BurstType burst = BurstType::Increment;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> byteEnable;

    std::size_t beatCount() const noexcept;
    bool valid(std::string &error) const;
};

struct MemoryResponse
{
    std::uint64_t token = 0;
    std::uint32_t id = 0;
    std::vector<std::uint8_t> data;
    bool error = false;
};

// Consumes requests produced by an RTL initiator.
class TransactionBackend
{
  public:
    virtual bool canAccept(const MemoryRequest &request) const = 0;
    virtual bool submit(const MemoryRequest &request) = 0;
    virtual bool getResponse(MemoryResponse &response) = 0;
    virtual void advance() = 0;
    virtual ~TransactionBackend() = default;
};

// Produces requests for an RTL target and consumes its responses.
class TransactionSource
{
  public:
    virtual bool getRequest(MemoryRequest &request) = 0;
    virtual bool canAcceptResponse(const MemoryResponse &response) const = 0;
    virtual bool submitResponse(const MemoryResponse &response) = 0;
    virtual void advance() = 0;
    virtual ~TransactionSource() = default;
};

class BusTransactor
{
  public:
    virtual bool beforeClock() = 0;
    virtual bool afterClock() = 0;
    virtual bool isIdle() const = 0;
    virtual const char *getLastError() const = 0;
    virtual ~BusTransactor() = default;
};

} // namespace gem5::rtl_cosim

#endif // __RTL_COSIM_RUNTIME_TRANSACTION_HH__
