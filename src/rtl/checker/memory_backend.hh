/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_COSIM_CHECKER_MEMORY_BACKEND_HH__
#define __RTL_COSIM_CHECKER_MEMORY_BACKEND_HH__

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rtl/runtime/transaction.hh"

namespace gem5::rtl_cosim
{

class SparseMemory
{
  public:
    SparseMemory(std::uint64_t base, std::uint64_t size);
    bool contains(std::uint64_t address, std::size_t size) const noexcept;
    bool read(std::uint64_t address, std::uint8_t *data,
              std::size_t size) const noexcept;
    bool write(std::uint64_t address, const std::uint8_t *data,
               const std::uint8_t *enable, std::size_t size) noexcept;
    std::uint64_t
    base() const noexcept
    {
        return _base;
    }
    std::uint64_t
    size() const noexcept
    {
        return _size;
    }

  private:
    std::uint64_t _base;
    std::uint64_t _size;
    std::map<std::uint64_t, std::uint8_t> _bytes;
};

struct MemoryBackendConfig
{
    std::size_t latencyCycles = 1;
    std::size_t maxPending = 64;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> errorRanges;
};

class MemoryBackend : public TransactionBackend
{
  public:
    MemoryBackend(std::shared_ptr<SparseMemory> memory,
                  MemoryBackendConfig config = {});
    bool canAccept(const MemoryRequest &request) const override;
    bool submit(const MemoryRequest &request) override;
    bool getResponse(MemoryResponse &response) override;
    void advance() override;
    bool isIdle() const noexcept;
    std::uint64_t
    requests() const noexcept
    {
        return _requests;
    }
    std::uint64_t
    reads() const noexcept
    {
        return _reads;
    }
    std::uint64_t
    writes() const noexcept
    {
        return _writes;
    }

  private:
    struct Pending
    {
        std::uint64_t readyCycle;
        MemoryRequest request;
    };
    MemoryResponse execute(const MemoryRequest &request);
    bool errorsAt(std::uint64_t address, std::size_t size) const noexcept;

    std::shared_ptr<SparseMemory> _memory;
    MemoryBackendConfig _config;
    std::uint64_t _cycle = 0;
    std::uint64_t _requests = 0;
    std::uint64_t _reads = 0;
    std::uint64_t _writes = 0;
    std::deque<Pending> _pending;
    std::deque<MemoryResponse> _responses;
};

class ScriptedTransactionSource : public TransactionSource
{
  public:
    struct Expectation
    {
        bool error = false;
        std::optional<std::vector<std::uint8_t>> data;
    };

    explicit ScriptedTransactionSource(std::size_t maxOutstanding = 64);
    void add(MemoryRequest request);
    void add(MemoryRequest request, Expectation expectation);
    bool getRequest(MemoryRequest &request) override;
    bool canAcceptResponse(const MemoryResponse &response) const override;
    bool submitResponse(const MemoryResponse &response) override;
    void advance() override;
    bool getCompleted(MemoryResponse &response);
    bool isIdle() const noexcept;
    std::size_t
    requestCount() const noexcept
    {
        return _submitted;
    }
    std::size_t
    completedCount() const noexcept
    {
        return _completed;
    }
    std::size_t
    readCount() const noexcept
    {
        return _reads;
    }
    std::size_t
    writeCount() const noexcept
    {
        return _writes;
    }
    const std::string &
    error() const noexcept
    {
        return _error;
    }

  private:
    struct Queued
    {
        MemoryRequest request;
        Expectation expectation;
    };
    struct Outstanding
    {
        std::uint32_t id = 0;
        bool write = false;
        std::size_t responseBytes = 0;
        Expectation expectation;
    };
    std::size_t _maxOutstanding;
    std::size_t _submitted = 0;
    std::size_t _completed = 0;
    std::size_t _reads = 0;
    std::size_t _writes = 0;
    std::deque<Queued> _requests;
    std::unordered_map<std::uint64_t, Outstanding> _outstanding;
    std::deque<MemoryResponse> _responses;
    std::string _error;
};

} // namespace gem5::rtl_cosim

#endif // __RTL_COSIM_CHECKER_MEMORY_BACKEND_HH__
