/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/runtime/model_loader.hh"

#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gem5::rtl_cosim
{

namespace
{

std::string
lastLibraryError()
{
#ifdef _WIN32
    return "Windows loader error " + std::to_string(GetLastError());
#else
    const char *message = dlerror();
    return message ? message : "unknown dynamic loader error";
#endif
}

void *
openLibrary(const char *path)
{
#ifdef _WIN32
    return reinterpret_cast<void *>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void *
findSymbol(void *library, const char *name)
{
#ifdef _WIN32
    return reinterpret_cast<void *>(
        GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
    dlerror();
    return dlsym(library, name);
#endif
}

void
closeLibrary(void *library)
{
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

} // anonymous namespace

ModelLoader::~ModelLoader()
{
    close();
}

bool
ModelLoader::open(const std::string &path)
{
    close();
    _library = openLibrary(path.c_str());
    if (!_library) {
        _error = "cannot load '" + path + "': " + lastLibraryError();
        return false;
    }

    _createManager = reinterpret_cast<CreateManager>(
        findSymbol(_library, "createRtlCoreManagerV1"));
    if (!_createManager) {
        _error =
            "missing V1 factory createRtlCoreManagerV1: " + lastLibraryError();
        close();
        return false;
    }
    _destroyManager = reinterpret_cast<DestroyManager>(
        findSymbol(_library, "destroyRtlCoreManagerV1"));
    if (!_destroyManager) {
        _error = "missing V1 destructor destroyRtlCoreManagerV1: " +
                 lastLibraryError();
        close();
        return false;
    }

    _manager = _createManager();
    if (!_manager) {
        _error = "createRtlCoreManagerV1 returned nullptr";
        close();
        return false;
    }
    _error.clear();
    return true;
}

bool
ModelLoader::createCore(const std::string &configJson)
{
    if (!_manager) {
        _error = "no vendor library is open";
        return false;
    }
    if (_core) {
        _error = "a core has already been created";
        return false;
    }
    _core = _manager->createCore(configJson.c_str());
    if (!_core) {
        const char *message = _manager->getLastError();
        _error = "vendor failed to create core";
        if (message && *message) {
            _error += ": " + std::string(message);
        }
        return false;
    }
    _error.clear();
    return true;
}

void
ModelLoader::close() noexcept
{
    if (_manager && _core) {
        _manager->destroyCore(std::exchange(_core, nullptr));
    }
    if (_destroyManager && _manager) {
        _destroyManager(std::exchange(_manager, nullptr));
    }
    if (_library) {
        closeLibrary(std::exchange(_library, nullptr));
    }
    _createManager = nullptr;
    _destroyManager = nullptr;
}

} // namespace gem5::rtl_cosim
