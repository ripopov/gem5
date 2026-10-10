/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "renderer.h"

static _Alignas(8) uint8_t scene[56 + 24 * 2 + 40 * 2];
static _Alignas(8) uint8_t encoded[56];
static _Alignas(8) uint8_t output[96 * 96 * 4];
static uint8_t expected[sizeof(output)];

static Job
fixture(void)
{
    SceneHeader h = {2, 2, 70, 67, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 60};
    Material m = {{0.5f, 0.5f, 0.5f}, {0.25f, 0.5f, 0.75f}};
    Object a = {0, {-10, -10, 2}, {10, -10, 2}, {10, 10, 2}};
    Object b = {1, {-10, -10, 2}, {10, 10, 2}, {-10, 10, 2}};
    encode_header(scene, &h);
    encode_material(scene + 56, &m);
    encode_material(scene + 80, &m);
    encode_object(scene + 104, &a);
    encode_object(scene + 144, &b);
    assert(load_scene(scene, sizeof(scene)) == 0);
    return (Job){(uintptr_t)scene,
                 (uintptr_t)output,
                 70 * 4 + 8,
                 0,
                 0,
                 70,
                 67,
                 0,
                 4,
                 3,
                 UINT64_C(0xfedcba9876543210)};
}

static unsigned row_writes, fail_row;
static int
capture_dma_row(uint64_t address, const uint8_t *row, uint32_t length)
{
    assert(address >= 0x81002000);
    uint64_t offset = address - 0x81002000;
    assert(offset + length <= sizeof(output));
    assert(length && length <= TILE_SIZE * 4);
    ++row_writes;
    if (row_writes == fail_row) {
        return 20;
    }
    memcpy(output + offset, row, length);
    return 0;
}
static void
test_local_dma_renderer(void)
{
    Job job = fixture();
    memset(output, 0xa5, sizeof(output));
    encode_job(encoded, &job);
    assert(render(encoded, sizeof(scene)) == 0);
    memcpy(expected, output, sizeof(output));
    /* The bus addresses are deliberately not process pointers. Rendering
     * must use the local descriptor copy and emit rows via the DMA callback.
     */
    job.scene_addr = 0x80001000;
    job.output_addr = 0x81002000;
    encode_job(encoded, &job);
    memset(output, 0xa5, sizeof(output));
    row_writes = fail_row = 0;
    assert(render_local(encoded, scene, sizeof(scene), 0x81000000,
                        capture_dma_row) == 0);
    assert(row_writes == 2 * 67);
    assert(memcmp(output, expected, sizeof(output)) == 0);
    job.width = 71;
    encode_job(encoded, &job);
    row_writes = 0;
    assert(render_local(encoded, scene, sizeof(scene), 0x81000000,
                        capture_dma_row) != 0);
    assert(row_writes == 0);
    job.width = 70;
    encode_job(encoded, &job);
    row_writes = 0;
    fail_row = 2;
    assert(render_local(encoded, scene, sizeof(scene), 0x81000000,
                        capture_dma_row) == 20);
    assert(row_writes == 2);
    fail_row = 0;
}

static void
test_path_energy(void)
{
    static _Alignas(8) uint8_t room[56 + 24 + 40 * 12];
    const Vec vertices[8] = {{-1, -1, -1}, {1, -1, -1}, {-1, 1, -1},
                             {1, 1, -1},   {-1, -1, 1}, {1, -1, 1},
                             {-1, 1, 1},   {1, 1, 1}};
    const unsigned faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1},
                                  {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    SceneHeader h = {1, 12, 8, 8, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 60};
    Material m = {{0.5f, 0.5f, 0.5f}, {0.125f, 0.0625f, 0.03125f}};
    encode_header(room, &h);
    encode_material(room + 56, &m);
    for (unsigned i = 0; i < 6; ++i) {
        Object a = {0, vertices[faces[i][0]], vertices[faces[i][1]],
                    vertices[faces[i][2]]};
        Object b = {0, vertices[faces[i][0]], vertices[faces[i][2]],
                    vertices[faces[i][3]]};
        encode_object(room + 80 + 80 * i, &a);
        encode_object(room + 120 + 80 * i, &b);
    }
    Job j = {
        (uintptr_t)room, (uintptr_t)output, 8 * 4, 0, 0, 8, 8, 0, 4, 0, 123};
    /* Closed emitting room: every path has known geometric-series energy. */
    float factor = 0, weight = 1;
    for (unsigned depth = 0; depth <= 4; ++depth) {
        factor += weight;
        weight *= 0.5f;
        j.depth = depth;
        encode_job(encoded, &j);
        assert(render(encoded, sizeof(room)) == 0);
        for (unsigned i = 0; i < 8 * 8; ++i) {
            assert(output[4 * i] == (unsigned)(255 * 0.125f * factor + 0.5f));
            assert(output[4 * i + 1] ==
                   (unsigned)(255 * 0.0625f * factor + 0.5f));
            assert(output[4 * i + 2] ==
                   (unsigned)(255 * 0.03125f * factor + 0.5f));
            assert(output[4 * i + 3] == 255);
        }
    }
}

int
main(int argc, char **argv)
{
    Job j = fixture(), decoded;
    encode_job(encoded, &j);
    decode_job(encoded, &decoded);
    assert(memcmp(&j, &decoded, 56) == 0);
    assert(encoded[48] == 0x10 && encoded[55] == 0xfe);
    assert(read32(scene + 8) == 70 && read_float(scene + 52) == 60);
    /* Analytic distances, shared-edge ties, reverse-side intersections. */
    for (int y = -5; y <= 5; ++y) {
        for (int x = -5; x <= 5; ++x) {
            Vec d = {x * 0.1f, y * 0.1f, 1};
            float a, b;
            int ia = trace_test((Vec){0, 0, 0}, d, 0, &a);
            int ib = trace_test((Vec){0, 0, 0}, d, 1, &b);
            assert(ia == ib && fabsf(a - b) < 1e-6f && a == 2);
        }
    }
    float distance;
    assert(trace_test((Vec){0, 0, 3}, (Vec){0, 0, -1}, 0, &distance) == 0);
    assert(distance == 1);
    assert(trace_test((Vec){0, 0, 0}, (Vec){0, 0, -1}, 0, &distance) == -1);
    memset(output, 0xa5, sizeof(output));
    assert(render(encoded, sizeof(scene)) == 0);
    for (unsigned y = 0; y < 67; ++y) {
        for (unsigned x = 0; x < 70; ++x) {
            uint8_t *p = output + y * j.stride + 4 * x;
            assert(p[0] == 64 && p[1] == 128 && p[2] == 191 && p[3] == 255);
        }
        for (unsigned b = 280; b < j.stride; ++b) {
            assert(output[y * j.stride + b] == 0xa5);
        }
    }
    memcpy(expected, output, sizeof(output));
    memset(output, 0xa5, sizeof(output));
    /* Host partitions need not align with the kernel's 64x64 subtiles. */
    for (unsigned y = 0; y < 67; y += 17) {
        for (unsigned x = 0; x < 70; x += 23) {
            j.x = x;
            j.y = y;
            j.width = 70 - x < 23 ? 70 - x : 23;
            j.height = 67 - y < 17 ? 67 - y : 17;
            encode_job(encoded, &j);
            assert(render(encoded, sizeof(scene)) == 0);
        }
    }
    assert(memcmp(expected, output, sizeof(output)) == 0);
    j = fixture();
    j.mode = 1;
    j.samples = 1;
    j.depth = 0;
    encode_job(encoded, &j);
    assert(render(encoded, sizeof(scene)) == 0);
    assert(memcmp(expected, output, sizeof(output)) == 0);
    j = fixture();
    j.width = UINT32_MAX;
    assert(validate_job(&j));
    j = fixture();
    j.stride = 279;
    assert(validate_job(&j));
    j = fixture();
    j.scene_addr++;
    assert(validate_job(&j));
    j = fixture();
    j.output_addr = j.scene_addr;
    assert(validate_job(&j));
    j = fixture();
    j.output_addr = UINT64_MAX - 3;
    assert(validate_job(&j));
    j = fixture();
    j.mode = 2;
    assert(validate_job(&j));
    j = fixture();
    j.samples = 0;
    assert(validate_job(&j));
    j = fixture();
    j.samples = MAX_SAMPLES + 1;
    assert(validate_job(&j));
    j = fixture();
    j.samples = MAX_SAMPLES;
    j.depth = MAX_DEPTH;
    assert(validate_job(&j) == 0);
    j = fixture();
    j.depth = MAX_DEPTH + 1;
    assert(validate_job(&j));
    j = fixture();
    j.mode = 1;
    assert(validate_job(&j));
    j = fixture();
    write32(scene, MAX_MATERIALS + 1);
    assert(load_scene(scene, sizeof(scene)));
    j = fixture();
    write32(scene + 4, MAX_OBJECTS + 1);
    assert(load_scene(scene, sizeof(scene)));
    j = fixture();
    write_float(scene + 52, NAN);
    assert(load_scene(scene, sizeof(scene)));
    j = fixture();
    write_float(scene + 40, 0);
    write_float(scene + 44, 0);
    assert(load_scene(scene, sizeof(scene)));
    j = fixture();
    write_float(scene + 56, -0.1f);
    assert(load_scene(scene, sizeof(scene)));
    j = fixture();
    write32(scene + 104, 2);
    assert(load_scene(scene, sizeof(scene)));
    j = fixture();
    memcpy(scene + 120, scene + 108, 12);
    assert(load_scene(scene, sizeof(scene)));
    j = fixture();
    assert(load_scene(scene, sizeof(scene) - 1));
    test_local_dma_renderer();
    test_path_energy();
    if (argc == 2) {
        static _Alignas(8) uint8_t complex_scene[SCENE_CAPACITY];
        FILE *file = fopen(argv[1], "rb");
        assert(file);
        size_t size = fread(complex_scene, 1, sizeof(complex_scene), file);
        assert(!ferror(file) && fclose(file) == 0);
        assert(load_scene(complex_scene, size) == 0);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 100; ++x) {
                Vec direction = {(x - 49.5f) / 100, (y - 49.5f) / 100, 1};
                float a, b;
                int ia = trace_test((Vec){0, 1, -3.15f}, direction, 0, &a);
                int ib = trace_test((Vec){0, 1, -3.15f}, direction, 1, &b);
                assert(ia == ib && fabsf(a - b) < 1e-6f);
            }
        }
        j = (Job){(uintptr_t)complex_scene,
                  (uintptr_t)output,
                  96 * 4,
                  0,
                  0,
                  96,
                  96,
                  0,
                  2,
                  3,
                  123};
        encode_job(encoded, &j);
        assert(render(encoded, size) == 0);
        memcpy(expected, output, sizeof(output));
        memset(output, 0xcc, sizeof(output));
        for (int y = 95 / 19 * 19; y >= 0; y -= 19) {
            for (int x = 95 / 13 * 13; x >= 0; x -= 13) {
                j.x = x;
                j.y = y;
                j.width = 96 - x < 13 ? 96 - x : 13;
                j.height = 96 - y < 19 ? 96 - y : 19;
                encode_job(encoded, &j);
                assert(render(encoded, size) == 0);
            }
        }
        assert(memcmp(expected, output, sizeof(output)) == 0);
        j.x = 0;
        j.y = 0;
        j.width = 96;
        j.height = 96;
        j.mode = 1;
        j.samples = 1;
        j.depth = 0;
        j.seed = 1;
        encode_job(encoded, &j);
        assert(render(encoded, size) == 0);
        memcpy(expected, output, sizeof(output));
        j.seed = UINT64_MAX;
        encode_job(encoded, &j);
        assert(render(encoded, size) == 0);
        assert(memcmp(expected, output, sizeof(output)) == 0);
    }
    puts("PASS: layout, malformed descriptors, intersections, modes, tiling");
    return 0;
}
