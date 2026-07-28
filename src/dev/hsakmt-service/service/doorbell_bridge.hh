#ifndef __DEV_HSAKMT_SERVICE_DOORBELL_BRIDGE_HH__
#define __DEV_HSAKMT_SERVICE_DOORBELL_BRIDGE_HH__

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace gem5_hsakmt
{

struct DoorbellRecord
{
    uint32_t processId = 0;
    uint32_t queueId = 0;
    uint32_t offset = 0;
    uint64_t value = 0;
};

/** Validates watcher records before they become modeled MMIO writes. */
class DoorbellBridge
{
  public:
    using Sink = std::function<void(uint32_t, uint64_t)>;

    explicit DoorbellBridge(Sink sink);
    bool registerQueue(uint32_t process_id, uint32_t queue_id,
                       uint32_t offset);
    bool unregisterQueue(uint32_t process_id, uint32_t queue_id);
    bool deliver(const DoorbellRecord &record);
    void clear();

    size_t
    queueCount() const
    {
        return queues.size();
    }

  private:
    static uint64_t key(uint32_t process_id, uint32_t queue_id);

    Sink sink;
    std::unordered_map<uint64_t, uint32_t> queues;
};

} // namespace gem5_hsakmt

#endif // __DEV_HSAKMT_SERVICE_DOORBELL_BRIDGE_HH__
