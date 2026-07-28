#include "dev/hsakmt-service/service/doorbell_bridge.hh"

#include <stdexcept>
#include <utility>

namespace gem5_hsakmt
{

DoorbellBridge::DoorbellBridge(Sink record_sink)
    : sink(std::move(record_sink))
{
    if (!sink) {
        throw std::invalid_argument("doorbell bridge requires a sink");
    }
}

uint64_t
DoorbellBridge::key(uint32_t process_id, uint32_t queue_id)
{
    return (static_cast<uint64_t>(process_id) << 32) | queue_id;
}

bool
DoorbellBridge::registerQueue(uint32_t process_id, uint32_t queue_id,
                              uint32_t offset)
{
    if (!process_id || !queue_id || (offset & 3)) {
        return false;
    }
    return queues.emplace(key(process_id, queue_id), offset).second;
}

bool
DoorbellBridge::unregisterQueue(uint32_t process_id, uint32_t queue_id)
{
    return queues.erase(key(process_id, queue_id)) != 0;
}

bool
DoorbellBridge::deliver(const DoorbellRecord &record)
{
    const auto queue = queues.find(key(record.processId, record.queueId));
    if (queue == queues.end() || queue->second != record.offset) {
        return false;
    }
    sink(record.offset, record.value);
    return true;
}

void
DoorbellBridge::clear()
{
    queues.clear();
}

} // namespace gem5_hsakmt
