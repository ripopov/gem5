#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

#include <hip/hip_runtime.h>

#define HIP_CHECK(call)                                                       \
    do {                                                                      \
        hipError_t status = (call);                                           \
        if (status != hipSuccess) {                                           \
            std::cerr << "HIP error: " << hipGetErrorString(status) << " at " \
                      << __FILE__ << ":" << __LINE__ << "\n";                 \
            return 1;                                                         \
        }                                                                     \
    } while (0)

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kPixelCount = kWidth * kHeight;

struct Pixel
{
    float r;
    float g;
    float b;
    float a;
};

__device__ float
clamp_color(float value)
{
    return fminf(1.0f, fmaxf(0.0f, value));
}

__global__ void
fill_rainbow(Pixel *pixels)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= kWidth || y >= kHeight) {
        return;
    }

    // A branch-free HSV color wheel runs left-to-right. A vertical brightness
    // ramp makes both image coordinates visible in the generated pattern.
    float hue = static_cast<float>(x) / static_cast<float>(kWidth - 1);
    float wheel = hue * 6.0f;
    float shade =
        0.5f + 0.5f * static_cast<float>(y) / static_cast<float>(kHeight - 1);
    // Four channels keep each pixel naturally aligned to a 16-byte GPU store.
    pixels[y * kWidth + x] = Pixel{
        shade * clamp_color(fabsf(wheel - 3.0f) - 1.0f),
        shade * clamp_color(2.0f - fabsf(wheel - 2.0f)),
        shade * clamp_color(2.0f - fabsf(wheel - 4.0f)),
        1.0f,
    };
}

std::uint8_t
to_byte(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
}

template <typename T>
void
write_le(std::ofstream &out, T value)
{
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.put(static_cast<char>((value >> (8 * i)) & 0xffu));
    }
}

bool
write_bmp(const char *path, const std::vector<Pixel> &pixels)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    constexpr std::uint32_t file_header_size = 14;
    constexpr std::uint32_t dib_header_size = 40;
    constexpr std::uint32_t row_stride = ((kWidth * 3u + 3u) / 4u) * 4u;
    constexpr std::uint32_t pixel_data_size = row_stride * kHeight;
    constexpr std::uint32_t file_size =
        file_header_size + dib_header_size + pixel_data_size;

    out.put('B');
    out.put('M');
    write_le<std::uint32_t>(out, file_size);
    write_le<std::uint16_t>(out, 0);
    write_le<std::uint16_t>(out, 0);
    write_le<std::uint32_t>(out, file_header_size + dib_header_size);

    write_le<std::uint32_t>(out, dib_header_size);
    write_le<std::uint32_t>(out, kWidth);
    write_le<std::uint32_t>(out, kHeight);
    write_le<std::uint16_t>(out, 1);
    write_le<std::uint16_t>(out, 24);
    write_le<std::uint32_t>(out, 0);
    write_le<std::uint32_t>(out, pixel_data_size);
    write_le<std::uint32_t>(out, 2835);
    write_le<std::uint32_t>(out, 2835);
    write_le<std::uint32_t>(out, 0);
    write_le<std::uint32_t>(out, 0);

    const char padding[3] = {0, 0, 0};
    for (int y = kHeight - 1; y >= 0; --y) {
        for (int x = 0; x < kWidth; ++x) {
            const Pixel &pixel = pixels[y * kWidth + x];
            out.put(static_cast<char>(to_byte(pixel.b)));
            out.put(static_cast<char>(to_byte(pixel.g)));
            out.put(static_cast<char>(to_byte(pixel.r)));
        }
        out.write(padding, row_stride - kWidth * 3u);
    }
    return static_cast<bool>(out);
}

std::uint64_t
checksum_pixels(const std::vector<Pixel> &pixels)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const Pixel &pixel : pixels) {
        hash = (hash ^ to_byte(pixel.r)) * 1099511628211ull;
        hash = (hash ^ to_byte(pixel.g)) * 1099511628211ull;
        hash = (hash ^ to_byte(pixel.b)) * 1099511628211ull;
    }
    return hash;
}

int
main()
{
    std::vector<Pixel> pixels(kPixelCount);
    Pixel *device_pixels = nullptr;
    HIP_CHECK(hipMalloc(&device_pixels, pixels.size() * sizeof(Pixel)));

    dim3 block(16, 16);
    dim3 grid((kWidth + block.x - 1) / block.x,
              (kHeight + block.y - 1) / block.y);
    hipLaunchKernelGGL(fill_rainbow, grid, block, 0, 0, device_pixels);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipMemcpy(pixels.data(), device_pixels,
                        pixels.size() * sizeof(Pixel), hipMemcpyDeviceToHost));
    HIP_CHECK(hipFree(device_pixels));

    const char *guest_path = "/tmp/rainbow-image.bmp";
    if (!write_bmp(guest_path, pixels)) {
        std::cerr << "rainbow_image: FAIL could not write " << guest_path
                  << "\n";
        return 2;
    }

    std::cout << "rainbow_image: filled " << kWidth << "x" << kHeight
              << " checksum=0x" << std::hex << checksum_pixels(pixels)
              << std::dec << "\n";

    int rc = std::system("if [ -x /sbin/m5 ]; then /sbin/m5 writefile "
                         "/tmp/rainbow-image.bmp rainbow-image.bmp; fi");
    if (rc != 0) {
        std::cerr << "rainbow_image: FAIL m5 writefile rc=" << rc << "\n";
        return 3;
    }

    std::cout << "rainbow_image: PASS image=rainbow-image.bmp\n";
    return 0;
}
