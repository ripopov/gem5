/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <math.h>
#include <stdio.h>

#include "renderer.h"

#ifdef NATIVE
#include <stdlib.h>

#else
#include <gem5/m5ops.h>

#endif

#ifndef WIDTH
#define WIDTH 96
#endif
#ifndef HEIGHT
#define HEIGHT 96
#endif
_Static_assert(
    WIDTH > 0 && WIDTH % 4 == 0 && HEIGHT > 0,
    "Image dimensions require positive, four-byte-aligned BMP rows");
#define MATERIAL_COUNT 8
#define PI 3.14159265358979323846f

#ifdef ACCEL_HOST
#define DMA_DATA __attribute__((section(".dma"), aligned(64)))
#else
#define DMA_DATA
#endif
static DMA_DATA _Alignas(8) uint8_t scene_bytes[SCENE_CAPACITY];
static DMA_DATA _Alignas(8) uint8_t job_bytes[56];
static DMA_DATA _Alignas(8) uint8_t pixels[WIDTH * HEIGHT * 4];
#ifdef ACCEL_HOST
#include "host_runtime.h"

#define MAX_ACCELERATORS 64
static DMA_DATA _Alignas(64) uint8_t jobs[MAX_ACCELERATORS][64];
static uint32_t accelerator_cores;
#ifdef ACCEL_TEST
static uint64_t
image_hash(void)
{
    uint64_t hash = 0;
    for (unsigned i = 0; i < sizeof(pixels); ++i) {
        hash = 33 * hash + *(volatile uint8_t *)(pixels + i);
    }
    return hash;
}
static void
check_image_hash(uint64_t expected)
{
    if (image_hash() != expected) {
        m5_fail(0, 120);
        for (;;) {}
    }
}
#endif
static void
run_accelerators(Job *job)
{
    const uint8_t *probe = NULL;
#ifdef ACCEL_TEST
    probe = scene_bytes;
#endif
    /* DMA allocations use an uncached PMA region, including shared edge
     * cache lines. No cached aliases exist for descriptors or pixels. */
    __asm__ volatile("csrw mie, %0" : : "r"(1UL << 11) : "memory");
    accelerator_cores = *(volatile uint32_t *)0x20000108UL;
    if (!accelerator_cores || accelerator_cores > MAX_ACCELERATORS) {
        m5_fail(0, 94);
        for (;;) {}
    }
    unsigned columns = 1;
    while (2 * columns * columns < accelerator_cores) {
        columns *= 2;
    }
    unsigned rows = accelerator_cores / columns;
    unsigned index = 0;
    for (unsigned row = 0; row < rows; ++row) {
        unsigned count =
            accelerator_cores / rows + (row < accelerator_cores % rows);
        for (unsigned column = 0; column < count; ++column) {
            job->x = WIDTH * column / count;
            job->y = HEIGHT * row / rows;
#ifdef ACCEL_TEST
            job->width = job->height = 1;
#else
            job->width = WIDTH * (column + 1) / count - job->x;
            job->height = HEIGHT * (row + 1) / rows - job->y;
#endif
            encode_job(jobs[index++], job);
        }
    }
    /* Consume output only after every discovered accelerator completes. */
    host_submit_and_wait(jobs, accelerator_cores, 2, probe);
#ifdef ACCEL_TEST
    uint8_t saved[4 * MAX_ACCELERATORS];
    uint64_t expected_hash = image_hash();
    for (unsigned i = 0; i < accelerator_cores; ++i) {
        uint32_t offset =
            4 * (read32(jobs[i] + 20) + WIDTH * read32(jobs[i] + 24));
        for (unsigned b = 0; b < 4; ++b) {
            saved[4 * i + b] = pixels[offset + b];
        }
        write32(jobs[i] + 36, 2);
    }
    /* Unknown mode and excessive capacity must fail before output writes. */
    host_submit_and_wait(jobs, accelerator_cores, 3, probe);
    check_image_hash(expected_hash);
    for (unsigned i = 0; i < accelerator_cores; ++i) {
        write32(jobs[i] + 36, 1);
    }
    write32(scene_bytes, MAX_MATERIALS + 1);
    host_submit_and_wait(jobs, accelerator_cores, 3, probe);
    check_image_hash(expected_hash);
    write32(scene_bytes, MATERIAL_COUNT);
    host_submit_and_wait(jobs, accelerator_cores, 2, probe);
    for (unsigned i = 0; i < accelerator_cores; ++i) {
        uint32_t offset =
            4 * (read32(jobs[i] + 20) + WIDTH * read32(jobs[i] + 24));
        for (unsigned b = 0; b < 4; ++b) {
            if (saved[4 * i + b] != pixels[offset + b]) {
                m5_fail(0, 120);
                for (;;) {}
            }
        }
    }
    m5_write_file(saved, 4 * accelerator_cores, 0, "protocol.bin");
#endif
}

#endif
static _Alignas(8) uint8_t parameters[24];
static uint8_t bmp_header[54], bmp_row[WIDTH * 3];
static uint32_t object_count;
static size_t scene_size;

static void
stop(int code)
{
#ifdef NATIVE
    exit(code);
#else
    m5_fail(0, code);
    for (;;) {}
#endif
}
static void
export_file(void *p, size_t size, uint64_t offset, const char *name)
{
#ifdef NATIVE
    FILE *file = fopen(name, offset ? "r+b" : "wb");
    if (!file || fseek(file, offset, SEEK_SET) ||
        fwrite(p, 1, size, file) != size || fclose(file)) {
        stop(90);
    }
#else
    if (m5_write_file(p, size, offset, name) != size) {
        stop(90);
    }
#endif
}
static Vec
v(float x, float y, float z)
{
    return (Vec){x, y, z};
}
static void
triangle(uint32_t material, Vec a, Vec b, Vec c)
{
    if (object_count == MAX_OBJECTS) {
        stop(91);
    }
    Object o = {material, a, b, c};
    encode_object(scene_bytes + 56 + 24 * MATERIAL_COUNT + 40 * object_count++,
                  &o);
}
static void
quad(uint32_t material, Vec a, Vec b, Vec c, Vec d)
{
    triangle(material, a, b, c);
    triangle(material, a, c, d);
}
static Vec
box_vertex(float x, float y, float z, float cx, float cz, float angle)
{
    return v(cx + x * cosf(angle) + z * sinf(angle), y,
             cz - x * sinf(angle) + z * cosf(angle));
}
static void
box(float cx, float cz, float width, float height, float depth, float angle)
{
    Vec p[8];
    for (int i = 0; i < 8; ++i) {
        p[i] = box_vertex((i & 1 ? 1 : -1) * width / 2, i & 2 ? height : 0,
                          (i & 4 ? 1 : -1) * depth / 2, cx, cz, angle);
    }
    quad(0, p[0], p[1], p[3], p[2]);
    quad(0, p[4], p[6], p[7], p[5]);
    quad(0, p[0], p[4], p[5], p[1]);
    quad(0, p[2], p[3], p[7], p[6]);
    quad(0, p[0], p[2], p[6], p[4]);
    quad(0, p[1], p[5], p[7], p[3]);
}
static Vec
sphere_vertex(Vec center, float radius, int latitude, int longitude)
{
    float theta = PI * latitude / 6, phi = 2 * PI * longitude / 12;
    /* Exact poles avoid almost-degenerate triangles. */
    if (latitude == 0) {
        return v(center.x, center.y + radius, center.z);
    }
    if (latitude == 6) {
        return v(center.x, center.y - radius, center.z);
    }
    return v(center.x + radius * sinf(theta) * cosf(phi),
             center.y + radius * cosf(theta),
             center.z + radius * sinf(theta) * sinf(phi));
}
static void
sphere(uint32_t material, Vec center, float radius)
{
    for (int lat = 0; lat < 6; ++lat) {
        for (int lon = 0; lon < 12; ++lon) {
            Vec a = sphere_vertex(center, radius, lat, lon);
            Vec b = sphere_vertex(center, radius, lat, lon + 1);
            Vec c = sphere_vertex(center, radius, lat + 1, lon + 1);
            Vec d = sphere_vertex(center, radius, lat + 1, lon);
            if (lat > 0) {
                triangle(material, a, b, c);
            }
            if (lat < 5) {
                triangle(material, a, c, d);
            }
        }
    }
}
static void
create_scene(void)
{
    const Material m[MATERIAL_COUNT] = {{{0.73f, 0.73f, 0.73f}, {0, 0, 0}},
                                        {{0.65f, 0.055f, 0.045f}, {0, 0, 0}},
                                        {{0.055f, 0.55f, 0.10f}, {0, 0, 0}},
                                        {{0, 0, 0}, {3.0f, 2.8f, 2.5f}},
                                        {{0.13f, 0.26f, 0.68f}, {0, 0, 0}},
                                        {{0.75f, 0.48f, 0.09f}, {0, 0, 0}},
                                        {{0.73f, 0.73f, 0.73f}, {0, 0, 0}},
                                        {{0, 0, 0}, {0.5f, 0.7f, 1.0f}}};
    for (int i = 0; i < MATERIAL_COUNT; ++i) {
        encode_material(scene_bytes + 56 + 24 * i, &m[i]);
    }
    /* Open front at z=0, colored side walls, floor/ceiling/back. */
    quad(0, v(-1, 0, 0), v(1, 0, 0), v(1, 0, 2), v(-1, 0, 2));
    quad(0, v(-1, 2, 0), v(-1, 2, 2), v(1, 2, 2), v(1, 2, 0));
    quad(0, v(-1, 0, 2), v(1, 0, 2), v(1, 2, 2), v(-1, 2, 2));
    quad(1, v(-1, 0, 0), v(-1, 0, 2), v(-1, 2, 2), v(-1, 2, 0));
    quad(2, v(1, 0, 0), v(1, 2, 0), v(1, 2, 2), v(1, 0, 2));
    quad(3, v(-0.60f, 1.995f, 0.65f), v(0.60f, 1.995f, 0.65f),
         v(0.60f, 1.995f, 1.55f), v(-0.60f, 1.995f, 1.55f));
    /* A second small, cool panel exercises multiple emission sources. */
    quad(7, v(-0.25f, 1.45f, 1.995f), v(0.25f, 1.45f, 1.995f),
         v(0.25f, 1.75f, 1.995f), v(-0.25f, 1.75f, 1.995f));
    box(-0.48f, 1.30f, 0.58f, 0.75f, 0.60f, 0.28f);
    box(0.50f, 1.45f, 0.48f, 0.40f, 0.48f, -0.30f);
    sphere(4, v(-0.40f, 0.26f, 0.45f), 0.26f);
    sphere(5, v(0.43f, 0.65f, 1.43f), 0.25f);
    SceneHeader header = {MATERIAL_COUNT,     object_count, WIDTH,     HEIGHT,
                          {0, 1.02f, -3.15f}, {0, 1.0f, 1}, {0, 1, 0}, 38};
    encode_header(scene_bytes, &header);
    scene_size = 56 + 24 * MATERIAL_COUNT + 40 * object_count;
}
static void
export_bmp(void)
{
    /* 24-bit bottom-up BMP, BGR rows. WIDTH*3 is aligned to four. */
    bmp_header[0] = 'B';
    bmp_header[1] = 'M';
    write32(bmp_header + 2, 54 + WIDTH * HEIGHT * 3);
    write32(bmp_header + 10, 54);
    write32(bmp_header + 14, 40);
    write32(bmp_header + 18, WIDTH);
    write32(bmp_header + 22, HEIGHT);
    bmp_header[26] = 1;
    bmp_header[28] = 24;
    write32(bmp_header + 34, WIDTH * HEIGHT * 3);
    export_file(bmp_header, 54, 0, "image.bmp");
    for (uint32_t row = 0; row < HEIGHT; ++row) {
        for (uint32_t x = 0; x < WIDTH; ++x) {
            const uint8_t *p = pixels + 4 * ((HEIGHT - 1 - row) * WIDTH + x);
            bmp_row[3 * x] = p[2];
            bmp_row[3 * x + 1] = p[1];
            bmp_row[3 * x + 2] = p[0];
        }
        export_file(bmp_row, sizeof(bmp_row), 54 + row * sizeof(bmp_row),
                    "image.bmp");
    }
}
#ifdef NATIVE
int
main(int argc, char **argv)
#else
int
main(void)
#endif
{
#ifdef NATIVE
    if (argc != 5) {
        fprintf(stderr, "usage: native mode samples depth seed\n");
        return 1;
    }
    write32(parameters, 0x52545031);
    for (int i = 1; i <= 3; ++i) {
        write32(parameters + 4 * i, strtoul(argv[i], NULL, 0));
    }
    write64(parameters + 16, strtoull(argv[4], NULL, 0));
#else
    if (m5_read_file(parameters, sizeof(parameters), 0) !=
        sizeof(parameters)) {
        stop(92);
    }
#endif
    if (read32(parameters) != 0x52545031) {
        stop(93);
    }
    create_scene();
    Job job = {(uintptr_t)scene_bytes,
               (uintptr_t)pixels,
               WIDTH * 4,
               0,
               0,
               WIDTH,
               HEIGHT,
               read32(parameters + 4),
               read32(parameters + 8),
               read32(parameters + 12),
               read64(parameters + 16)};
    encode_job(job_bytes, &job);
#ifdef ACCEL_HOST
    run_accelerators(&job);
    uint8_t core_count[4];
    write32(core_count, accelerator_cores);
    export_file(core_count, sizeof(core_count), 0, "core_count.bin");
    for (unsigned i = 0; i < accelerator_cores; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "job%u.bin", i);
        export_file(jobs[i], 56, 0, name);
    }
#else
    /* The consumer sees serialized data, never the producer's structures. */
#ifdef HOST_RENDER_STATS
    uint64_t stats_sync_ns = m5_init_param(0, 0) + 1;
    __asm__ volatile("fence iorw, iorw" : : : "memory");
    m5_reset_stats(0, 0);
    m5_quiesce_ns(stats_sync_ns);
#endif
    int error = render(job_bytes, scene_size);
    if (error) {
        stop(error);
    }
#ifdef HOST_RENDER_STATS
    __asm__ volatile("fence iorw, iorw" : : : "memory");
    m5_dump_reset_stats(0, 0);
    m5_quiesce_ns(stats_sync_ns);
#endif
#endif
    export_file(scene_bytes, scene_size, 0, "scene.bin");
    export_file(job_bytes, sizeof(job_bytes), 0, "job.bin");
    export_file(pixels, sizeof(pixels), 0, "image.rgba");
    export_bmp();
#ifdef NATIVE
    return 0;
#else
    m5_exit(0);
    for (;;) {}
#endif
}
