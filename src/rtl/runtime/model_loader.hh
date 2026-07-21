/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_COSIM_RUNTIME_MODEL_LOADER_HH__
#define __RTL_COSIM_RUNTIME_MODEL_LOADER_HH__

#include <string>

#include "gem5/rtl_cosim/api_v1.hh"

namespace gem5::rtl_cosim
{

class ModelLoader
{
  public:
    ModelLoader() = default;
    ~ModelLoader();
    ModelLoader(const ModelLoader &) = delete;
    ModelLoader &operator=(const ModelLoader &) = delete;

    bool open(const std::string &path);
    bool createCore(const std::string &configJson);
    void close() noexcept;

    RtlCoreManager *
    manager() const noexcept
    {
        return _manager;
    }
    RtlCore *
    core() const noexcept
    {
        return _core;
    }
    const std::string &
    error() const noexcept
    {
        return _error;
    }

  private:
    using CreateManager = RtlCoreManager *(*)() noexcept;
    using DestroyManager = void (*)(RtlCoreManager *) noexcept;

    void *_library = nullptr;
    CreateManager _createManager = nullptr;
    DestroyManager _destroyManager = nullptr;
    RtlCoreManager *_manager = nullptr;
    RtlCore *_core = nullptr;
    std::string _error;
};

} // namespace gem5::rtl_cosim

#endif // __RTL_COSIM_RUNTIME_MODEL_LOADER_HH__
