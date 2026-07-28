#ifndef __DEV_HSAKMT_SERVICE_GEM5_DEVICE_BACKEND_HH__
#define __DEV_HSAKMT_SERVICE_GEM5_DEVICE_BACKEND_HH__

#include <functional>

#include <rocjitsu/kmd/server.h>

namespace gem5
{
namespace hsa
{

/**
 * Converts rocjitsu's versioned C callbacks into checked C++ callables.
 *
 * The adapter owns no device state. It catches every exception before it can
 * cross the C ABI and invokes assertEventQueue for every operation that may
 * touch a SimObject.
 */
class Gem5DeviceBackend
{
  public:
    struct Handlers
    {
        std::function<void()> assertEventQueue;
        std::function<int32_t(const rj_kmd_process_request_t &)> openProcess;
        std::function<int32_t(const rj_kmd_process_request_t &)>
            setProcessClientPid;
        std::function<int32_t(const rj_kmd_process_request_t &)> closeProcess;
        std::function<int32_t(const rj_kmd_allocation_request_t &,
                              rj_kmd_backing_slice_t &)> allocate;
        std::function<int32_t(const rj_kmd_free_request_t &)> free;
        std::function<int32_t(const rj_kmd_mapping_t &)> map;
        std::function<int32_t(const rj_kmd_unmap_request_t &)> unmap;
        std::function<int32_t(const rj_kmd_mapping_t &)> setMappingMemoryType;
        std::function<int32_t(const rj_kmd_apertures_t &)> setApertures;
        std::function<int32_t(const rj_kmd_queue_t &)> createQueue;
        std::function<int32_t(const rj_kmd_queue_update_t &)> updateQueue;
        std::function<int32_t(const rj_kmd_queue_key_t &)> destroyQueue;
        std::function<int32_t(const rj_kmd_write64_t &)> write64;
        std::function<void()> shutdown;
    };

    explicit Gem5DeviceBackend(Handlers handlers);
    rj_kmd_backend_ops_t operations();

  private:
    template <typename Request, typename Handler>
    int32_t invoke(const Request *request, const Handler &handler);
    void checkEventQueue() const;

    static int32_t openProcess(void *context,
                               const rj_kmd_process_request_t *request);
    static int32_t setProcessClientPid(
        void *context, const rj_kmd_process_request_t *request);
    static int32_t closeProcess(void *context,
                                const rj_kmd_process_request_t *request);
    static int32_t allocate(void *context,
                            const rj_kmd_allocation_request_t *request,
                            rj_kmd_backing_slice_t *slice);
    static int32_t free(void *context,
                        const rj_kmd_free_request_t *request);
    static int32_t map(void *context, const rj_kmd_mapping_t *mapping);
    static int32_t unmap(void *context,
                         const rj_kmd_unmap_request_t *request);
    static int32_t setMappingMemoryType(
        void *context, const rj_kmd_mapping_t *mapping);
    static int32_t setApertures(void *context,
                                const rj_kmd_apertures_t *apertures);
    static int32_t createQueue(void *context,
                               const rj_kmd_queue_t *queue);
    static int32_t updateQueue(void *context,
                               const rj_kmd_queue_update_t *queue);
    static int32_t destroyQueue(void *context,
                                const rj_kmd_queue_key_t *queue);
    static int32_t write64(void *context,
                           const rj_kmd_write64_t *write);
    static void shutdown(void *context);

    Handlers handlers;
};

} // namespace hsa
} // namespace gem5

#endif // __DEV_HSAKMT_SERVICE_GEM5_DEVICE_BACKEND_HH__
