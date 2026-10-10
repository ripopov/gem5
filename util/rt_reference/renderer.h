/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RT_REFERENCE_RENDERER_H
#define RT_REFERENCE_RENDERER_H

#include <stddef.h>
#include <stdint.h>

#define MAX_MATERIALS 16
#define MAX_OBJECTS 512
#define MAX_DEPTH 32
#define MAX_SAMPLES 4096
#define TILE_SIZE 64
#define SCENE_CAPACITY (56 + 24 * MAX_MATERIALS + 40 * MAX_OBJECTS)

typedef struct
{
    float x, y, z;
} Vec;
typedef struct
{
    Vec albedo, emission;
} Material;
typedef struct
{
    uint32_t material;
    Vec v0, v1, v2;
} Object;
typedef struct
{
    uint32_t material_count, object_count, width, height;
    Vec position, target, up;
    float fov;
} SceneHeader;
typedef struct
{
    uint64_t scene_addr, output_addr;
    uint32_t stride, x, y, width, height;
    uint32_t mode, samples, depth;
    uint64_t seed;
} Job;

_Static_assert(sizeof(SceneHeader) == 56, "scene layout");
_Static_assert(sizeof(Material) == 24, "material layout");
_Static_assert(sizeof(Object) == 40, "object layout");
_Static_assert(sizeof(Job) == 56, "job layout");
_Static_assert(offsetof(Job, seed) == 48, "seed offset");

uint32_t read32(const uint8_t *p);
uint64_t read64(const uint8_t *p);
float read_float(const uint8_t *p);
void write32(uint8_t *p, uint32_t value);
void write64(uint8_t *p, uint64_t value);
void write_float(uint8_t *p, float value);
void encode_job(uint8_t *p, const Job *job);
void decode_job(const uint8_t *p, Job *job);
void encode_header(uint8_t *p, const SceneHeader *header);
void encode_material(uint8_t *p, const Material *material);
void encode_object(uint8_t *p, const Object *object);
int load_scene(const uint8_t *p, size_t size);
int validate_job(const Job *job);
int render(const uint8_t *job_bytes, size_t scene_size);
int render_local(const uint8_t *job_bytes, const uint8_t *scene_bytes,
                 size_t scene_size, uint64_t job_addr,
                 int (*write_row)(uint64_t, const uint8_t *, uint32_t));
int trace_test(Vec origin, Vec direction, int linear, float *distance);

#endif
