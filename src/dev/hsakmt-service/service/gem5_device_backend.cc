#include "dev/hsakmt-service/service/gem5_device_backend.hh"

#include <utility>

namespace gem5
{
namespace hsa
{
namespace
{

template <typename T>
bool
validRequest(const T *request)
{
    return request &&
        request->abi_version == RJ_KMD_SERVER_ABI_VERSION &&
        request->struct_size >= sizeof(T);
}

} // anonymous namespace

Gem5DeviceBackend::Gem5DeviceBackend(Handlers backend_handlers)
    : handlers(std::move(backend_handlers))
{
}

rj_kmd_backend_ops_t
Gem5DeviceBackend::operations()
{
    rj_kmd_backend_ops_t result = {};
    result.abi_version = RJ_KMD_SERVER_ABI_VERSION;
    result.struct_size = sizeof(result);
    result.context = this;
    result.open_process = &openProcess;
    result.set_process_client_pid = &setProcessClientPid;
    result.close_process = &closeProcess;
    result.allocate = &allocate;
    result.free = &free;
    result.map = &map;
    result.unmap = &unmap;
    result.set_mapping_memory_type = &setMappingMemoryType;
    result.set_apertures = &setApertures;
    result.create_queue = &createQueue;
    result.update_queue = &updateQueue;
    result.destroy_queue = &destroyQueue;
    result.write64 = &write64;
    result.shutdown = &shutdown;
    return result;
}

void
Gem5DeviceBackend::checkEventQueue() const
{
    if (handlers.assertEventQueue) {
        handlers.assertEventQueue();
    }
}

template <typename Request, typename Handler>
int32_t
Gem5DeviceBackend::invoke(const Request *request, const Handler &handler)
{
    if (!validRequest(request) || !handler) {
        return RJ_KMD_STATUS_INVALID_ARGUMENT;
    }
    try {
        checkEventQueue();
        return handler(*request);
    } catch (...) {
        return RJ_KMD_STATUS_INTERNAL_ERROR;
    }
}

int32_t
Gem5DeviceBackend::openProcess(
    void *context, const rj_kmd_process_request_t *request)
{
    return context
        ? static_cast<Gem5DeviceBackend *>(context)->invoke(
              request,
              static_cast<Gem5DeviceBackend *>(context)->handlers.openProcess)
        : RJ_KMD_STATUS_INVALID_ARGUMENT;
}

int32_t
Gem5DeviceBackend::setProcessClientPid(
    void *context, const rj_kmd_process_request_t *request)
{
    return context
        ? static_cast<Gem5DeviceBackend *>(context)->invoke(
              request, static_cast<Gem5DeviceBackend *>(context)
                           ->handlers.setProcessClientPid)
        : RJ_KMD_STATUS_INVALID_ARGUMENT;
}

int32_t
Gem5DeviceBackend::closeProcess(
    void *context, const rj_kmd_process_request_t *request)
{
    return context
        ? static_cast<Gem5DeviceBackend *>(context)->invoke(
              request,
              static_cast<Gem5DeviceBackend *>(context)->handlers.closeProcess)
        : RJ_KMD_STATUS_INVALID_ARGUMENT;
}

int32_t
Gem5DeviceBackend::allocate(
    void *context, const rj_kmd_allocation_request_t *request,
    rj_kmd_backing_slice_t *slice)
{
    if (!context || !validRequest(request) || !validRequest(slice)) {
        return RJ_KMD_STATUS_INVALID_ARGUMENT;
    }
    auto *self = static_cast<Gem5DeviceBackend *>(context);
    if (!self->handlers.allocate) {
        return RJ_KMD_STATUS_NOT_SUPPORTED;
    }
    try {
        self->checkEventQueue();
        return self->handlers.allocate(*request, *slice);
    } catch (...) {
        return RJ_KMD_STATUS_INTERNAL_ERROR;
    }
}

#define RJ_GEM5_CALLBACK(Name, Member, Type)                                  \
    int32_t Gem5DeviceBackend::Name(void *context, const Type *request)       \
    {                                                                         \
        return context                                                        \
            ? static_cast<Gem5DeviceBackend *>(context)->invoke(              \
                  request, static_cast<Gem5DeviceBackend *>(context)          \
                               ->handlers.Member)                             \
            : RJ_KMD_STATUS_INVALID_ARGUMENT;                                 \
    }

RJ_GEM5_CALLBACK(free, free, rj_kmd_free_request_t)
RJ_GEM5_CALLBACK(map, map, rj_kmd_mapping_t)
RJ_GEM5_CALLBACK(unmap, unmap, rj_kmd_unmap_request_t)
RJ_GEM5_CALLBACK(setMappingMemoryType, setMappingMemoryType,
                 rj_kmd_mapping_t)
RJ_GEM5_CALLBACK(setApertures, setApertures, rj_kmd_apertures_t)
RJ_GEM5_CALLBACK(createQueue, createQueue, rj_kmd_queue_t)
RJ_GEM5_CALLBACK(updateQueue, updateQueue, rj_kmd_queue_update_t)
RJ_GEM5_CALLBACK(destroyQueue, destroyQueue, rj_kmd_queue_key_t)
RJ_GEM5_CALLBACK(write64, write64, rj_kmd_write64_t)

#undef RJ_GEM5_CALLBACK

void
Gem5DeviceBackend::shutdown(void *context)
{
    if (!context) {
        return;
    }
    auto *self = static_cast<Gem5DeviceBackend *>(context);
    try {
        self->checkEventQueue();
        if (self->handlers.shutdown) {
            self->handlers.shutdown();
        }
    } catch (...) {
    }
}

} // namespace hsa
} // namespace gem5
