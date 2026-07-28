#include "dev/hsakmt-service/service/gem5_device_backend.hh"

#include <gtest/gtest.h>

#include <cerrno>
#include <stdexcept>
#include <utility>

namespace
{

template <typename T>
T
abiStruct()
{
    T value = {};
    value.abi_version = RJ_KMD_SERVER_ABI_VERSION;
    value.struct_size = sizeof(T);
    return value;
}

TEST(Gem5DeviceBackendTest, ValidatesAbiAndPreservesCallbackStatus)
{
    unsigned event_queue_checks = 0;
    gem5::hsa::Gem5DeviceBackend::Handlers handlers;
    handlers.assertEventQueue = [&] { ++event_queue_checks; };
    handlers.openProcess = [](const rj_kmd_process_request_t &) {
        return -EBUSY;
    };
    gem5::hsa::Gem5DeviceBackend backend(std::move(handlers));
    auto operations = backend.operations();
    auto request = abiStruct<rj_kmd_process_request_t>();
    request.process_id = 7;

    EXPECT_EQ(operations.open_process(operations.context, &request), -EBUSY);
    EXPECT_EQ(event_queue_checks, 1);

    request.abi_version++;
    EXPECT_EQ(operations.open_process(operations.context, &request),
              RJ_KMD_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(event_queue_checks, 1);
}

TEST(Gem5DeviceBackendTest, ReturnsSlicesAndContainsExceptions)
{
    unsigned event_queue_checks = 0;
    gem5::hsa::Gem5DeviceBackend::Handlers handlers;
    handlers.assertEventQueue = [&] { ++event_queue_checks; };
    handlers.allocate =
        [](const rj_kmd_allocation_request_t &request,
           rj_kmd_backing_slice_t &slice) {
            slice.allocation_id = 9;
            slice.device_paddr = 0x8000;
            slice.size = request.size;
            slice.fd = 17;
            slice.fd_offset = 0x3000;
            return 0;
        };
    handlers.free = [](const rj_kmd_free_request_t &) -> int32_t {
        throw std::runtime_error("contained C++ exception");
    };
    gem5::hsa::Gem5DeviceBackend backend(std::move(handlers));
    auto operations = backend.operations();

    auto request = abiStruct<rj_kmd_allocation_request_t>();
    request.size = 4096;
    auto slice = abiStruct<rj_kmd_backing_slice_t>();
    slice.fd = -1;
    ASSERT_EQ(operations.allocate(
                  operations.context, &request, &slice),
              0);
    EXPECT_EQ(slice.allocation_id, 9);
    EXPECT_EQ(slice.device_paddr, 0x8000);
    EXPECT_EQ(slice.size, 4096);
    EXPECT_EQ(slice.fd, 17);
    EXPECT_EQ(slice.fd_offset, 0x3000);

    auto release = abiStruct<rj_kmd_free_request_t>();
    EXPECT_EQ(operations.free(operations.context, &release),
              RJ_KMD_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(event_queue_checks, 2);
}

} // anonymous namespace
