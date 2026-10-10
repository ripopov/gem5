/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <math.h>

#include "renderer.h"

#define PI 3.14159265358979323846f
#define EPS 0.0001f
#define FAR 1.0e30f

typedef struct
{
    Vec vertex, edge1, edge2, normal, lo, hi, center;
    float area;
    uint32_t material;
} Triangle;
typedef struct
{
    Vec lo, hi;
    uint32_t left, right, first, count;
} Node;
typedef struct
{
    float distance;
    int object;
    Vec normal;
} Hit;

static SceneHeader scene;
static Material materials[MAX_MATERIALS];
static Triangle triangles[MAX_OBJECTS];
static Node nodes[2 * MAX_OBJECTS];
static uint32_t indices[MAX_OBJECTS], lights[MAX_OBJECTS];
static uint32_t node_count, light_count;
static Vec forward, camera_right, camera_up;
static float half_height;
static uint8_t tile[TILE_SIZE * TILE_SIZE * 4];

static Vec
vec(float x, float y, float z)
{
    return (Vec){x, y, z};
}
static Vec
add(Vec a, Vec b)
{
    return vec(a.x + b.x, a.y + b.y, a.z + b.z);
}
static Vec
sub(Vec a, Vec b)
{
    return vec(a.x - b.x, a.y - b.y, a.z - b.z);
}
static Vec
scale(Vec a, float s)
{
    return vec(a.x * s, a.y * s, a.z * s);
}
static Vec
mul(Vec a, Vec b)
{
    return vec(a.x * b.x, a.y * b.y, a.z * b.z);
}
static float
dot(Vec a, Vec b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static Vec
cross(Vec a, Vec b)
{
    return vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
               a.x * b.y - a.y * b.x);
}
static Vec
unit(Vec a)
{
    return scale(a, 1.0f / sqrtf(dot(a, a)));
}
static float
component(Vec a, int axis)
{
    return axis == 0 ? a.x : axis == 1 ? a.y : a.z;
}
static float
min_float(float a, float b)
{
    return a < b ? a : b;
}
static float
max_float(float a, float b)
{
    return a > b ? a : b;
}
static Vec
minimum(Vec a, Vec b)
{
    return vec(min_float(a.x, b.x), min_float(a.y, b.y), min_float(a.z, b.z));
}
static Vec
maximum(Vec a, Vec b)
{
    return vec(max_float(a.x, b.x), max_float(a.y, b.y), max_float(a.z, b.z));
}
static int
finite_vec(Vec a)
{
    return isfinite(a.x) && isfinite(a.y) && isfinite(a.z);
}
static int
nonzero(Vec a)
{
    return a.x > 0 || a.y > 0 || a.z > 0;
}

uint32_t
read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
uint64_t
read64(const uint8_t *p)
{
    return read32(p) | (uint64_t)read32(p + 4) << 32;
}
float
read_float(const uint8_t *p)
{
    union
    {
        uint32_t u;
        float f;
    } value = {.u = read32(p)};
    return value.f;
}
void
write32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        p[i] = v >> (8 * i);
    }
}
void
write64(uint8_t *p, uint64_t v)
{
    write32(p, v);
    write32(p + 4, v >> 32);
}
void
write_float(uint8_t *p, float v)
{
    union
    {
        uint32_t u;
        float f;
    } value = {.f = v};
    write32(p, value.u);
}
static Vec
read_vec(const uint8_t *p)
{
    return vec(read_float(p), read_float(p + 4), read_float(p + 8));
}
static void
write_vec(uint8_t *p, Vec v)
{
    write_float(p, v.x);
    write_float(p + 4, v.y);
    write_float(p + 8, v.z);
}
void
encode_header(uint8_t *p, const SceneHeader *s)
{
    write32(p, s->material_count);
    write32(p + 4, s->object_count);
    write32(p + 8, s->width);
    write32(p + 12, s->height);
    write_vec(p + 16, s->position);
    write_vec(p + 28, s->target);
    write_vec(p + 40, s->up);
    write_float(p + 52, s->fov);
}
void
encode_material(uint8_t *p, const Material *m)
{
    write_vec(p, m->albedo);
    write_vec(p + 12, m->emission);
}
void
encode_object(uint8_t *p, const Object *o)
{
    write32(p, o->material);
    write_vec(p + 4, o->v0);
    write_vec(p + 16, o->v1);
    write_vec(p + 28, o->v2);
}
void
encode_job(uint8_t *p, const Job *j)
{
    write64(p, j->scene_addr);
    write64(p + 8, j->output_addr);
    write32(p + 16, j->stride);
    write32(p + 20, j->x);
    write32(p + 24, j->y);
    write32(p + 28, j->width);
    write32(p + 32, j->height);
    write32(p + 36, j->mode);
    write32(p + 40, j->samples);
    write32(p + 44, j->depth);
    write64(p + 48, j->seed);
}
void
decode_job(const uint8_t *p, Job *j)
{
    j->scene_addr = read64(p);
    j->output_addr = read64(p + 8);
    j->stride = read32(p + 16);
    j->x = read32(p + 20);
    j->y = read32(p + 24);
    j->width = read32(p + 28);
    j->height = read32(p + 32);
    j->mode = read32(p + 36);
    j->samples = read32(p + 40);
    j->depth = read32(p + 44);
    j->seed = read64(p + 48);
}

static uint32_t
build_node(uint32_t first, uint32_t count)
{
    uint32_t id = node_count++;
    Node *n = &nodes[id];
    n->lo = vec(FAR, FAR, FAR);
    n->hi = vec(-FAR, -FAR, -FAR);
    Vec clo = n->lo, chi = n->hi;
    for (uint32_t i = first; i < first + count; ++i) {
        Triangle *t = &triangles[indices[i]];
        n->lo = minimum(n->lo, t->lo);
        n->hi = maximum(n->hi, t->hi);
        clo = minimum(clo, t->center);
        chi = maximum(chi, t->center);
    }
    n->first = first;
    n->count = count;
    if (count <= 4) {
        return id;
    }
    Vec extent = sub(chi, clo);
    int axis = extent.y > extent.x ? 1 : 0;
    if (extent.z > component(extent, axis)) {
        axis = 2;
    }
    /* Stable insertion sort and median split bound tree depth to nine. */
    for (uint32_t i = first + 1; i < first + count; ++i) {
        uint32_t index = indices[i], k = i;
        float c = component(triangles[index].center, axis);
        while (k > first &&
               component(triangles[indices[k - 1]].center, axis) > c) {
            indices[k] = indices[k - 1];
            --k;
        }
        indices[k] = index;
    }
    n->count = 0;
    n->first = axis;
    n->left = build_node(first, count / 2);
    n->right = build_node(first + count / 2, count - count / 2);
    return id;
}

int
load_scene(const uint8_t *p, size_t size)
{
    if (!p || (uintptr_t)p % 8 || size < 56) {
        return 1;
    }
    scene =
        (SceneHeader){read32(p),        read32(p + 4),     read32(p + 8),
                      read32(p + 12),   read_vec(p + 16),  read_vec(p + 28),
                      read_vec(p + 40), read_float(p + 52)};
    if (!scene.material_count || scene.material_count > MAX_MATERIALS ||
        !scene.object_count || scene.object_count > MAX_OBJECTS ||
        size != 56 + 24 * scene.material_count + 40 * scene.object_count ||
        !scene.width || !scene.height || !finite_vec(scene.position) ||
        !finite_vec(scene.target) || !finite_vec(scene.up) ||
        !isfinite(scene.fov) || scene.fov <= 0 || scene.fov >= 180) {
        return 2;
    }
    Vec f = sub(scene.target, scene.position), r = cross(f, scene.up);
    if (!finite_vec(f) || !finite_vec(r) || !isfinite(dot(f, f)) ||
        !isfinite(dot(r, r)) || dot(f, f) <= 0 || dot(r, r) <= 0) {
        return 3;
    }
    forward = unit(f);
    camera_right = unit(r);
    camera_up = cross(camera_right, forward);
    half_height = tanf(scene.fov * PI / 360);
    p += 56;
    for (uint32_t i = 0; i < scene.material_count; ++i, p += 24) {
        materials[i] = (Material){read_vec(p), read_vec(p + 12)};
        for (int axis = 0; axis < 3; ++axis) {
            float a = component(materials[i].albedo, axis);
            float e = component(materials[i].emission, axis);
            if (!isfinite(a) || !isfinite(e) || a < 0 || a > 1 || e < 0) {
                return 4;
            }
        }
    }
    light_count = 0;
    for (uint32_t i = 0; i < scene.object_count; ++i, p += 40) {
        Triangle *t = &triangles[i];
        Vec v0 = read_vec(p + 4), v1 = read_vec(p + 16), v2 = read_vec(p + 28);
        t->material = read32(p);
        if (t->material >= scene.material_count || !finite_vec(v0) ||
            !finite_vec(v1) || !finite_vec(v2)) {
            return 5;
        }
        t->vertex = v0;
        t->edge1 = sub(v1, v0);
        t->edge2 = sub(v2, v0);
        Vec n = cross(t->edge1, t->edge2);
        float length = sqrtf(dot(n, n));
        if (!isfinite(length) || length <= 0) {
            return 6;
        }
        t->normal = scale(n, 1 / length);
        t->area = length / 2;
        t->lo = minimum(v0, minimum(v1, v2));
        t->hi = maximum(v0, maximum(v1, v2));
        t->center = scale(add(v0, add(v1, v2)), 1.0f / 3);
        indices[i] = i;
        if (nonzero(materials[t->material].emission)) {
            lights[light_count++] = i;
        }
    }
    node_count = 0;
    build_node(0, scene.object_count);
    return 0;
}

int
validate_job(const Job *j)
{
    uint64_t output_size =
        (uint64_t)(scene.height - 1) * j->stride + 4ULL * scene.width;
    uint64_t scene_size =
        56 + 24 * scene.material_count + 40 * scene.object_count;
    if (!j->scene_addr || j->scene_addr % 8 || !j->output_addr ||
        j->output_addr % 4 || j->stride % 4 ||
        j->stride < 4ULL * scene.width || !j->width || !j->height ||
        (uint64_t)j->x + j->width > scene.width ||
        (uint64_t)j->y + j->height > scene.height || j->mode > 1 ||
        !j->samples || j->samples > MAX_SAMPLES || j->depth > MAX_DEPTH ||
        (j->mode == 1 && (j->samples != 1 || j->depth != 0)) ||
        j->output_addr > UINT64_MAX - output_size ||
        j->scene_addr > UINT64_MAX - scene_size) {
        return 7;
    }
    if (j->output_addr < j->scene_addr + scene_size &&
        j->scene_addr < j->output_addr + output_size) {
        return 8;
    }
    return 0;
}

static void
intersect_triangle(uint32_t id, Vec origin, Vec direction, Hit *hit)
{
    Triangle *t = &triangles[id];
    Vec p = cross(direction, t->edge2);
    float determinant = dot(t->edge1, p);
    if (fabsf(determinant) < 1.0e-8f) {
        return;
    }
    float inv = 1 / determinant;
    Vec s = sub(origin, t->vertex);
    float u = dot(s, p) * inv;
    if (u < 0 || u > 1) {
        return;
    }
    Vec q = cross(s, t->edge1);
    float v = dot(direction, q) * inv;
    if (v < 0 || u + v > 1) {
        return;
    }
    float d = dot(t->edge2, q) * inv;
    if (d <= EPS || d > hit->distance ||
        (d == hit->distance && hit->object >= 0 &&
         id >= (uint32_t)hit->object)) {
        return;
    }
    hit->distance = d;
    hit->object = id;
    hit->normal =
        dot(t->normal, direction) < 0 ? t->normal : scale(t->normal, -1);
}

static int
intersects_box(const Node *n, Vec origin, Vec inverse, float limit)
{
    float near = 0, far = limit;
    for (int a = 0; a < 3; ++a) {
        float o = component(origin, a), inv = component(inverse, a);
        float lo = component(n->lo, a) - EPS, hi = component(n->hi, a) + EPS;
        if (fabsf(inv) == FAR) {
            if (o < lo || o > hi) {
                return 0;
            }
        } else {
            float t0 = (lo - o) * inv, t1 = (hi - o) * inv;
            near = max_float(near, min_float(t0, t1));
            far = min_float(far, max_float(t0, t1));
            if (near > far) {
                return 0;
            }
        }
    }
    return 1;
}

static Hit
trace(Vec origin, Vec direction, float limit, int linear)
{
    Hit hit = {limit, -1, {0, 0, 0}};
    if (linear) {
        for (uint32_t i = 0; i < scene.object_count; ++i) {
            intersect_triangle(i, origin, direction, &hit);
        }
    } else {
        Vec inverse =
            vec(fabsf(direction.x) < 1.0e-20f ? FAR : 1 / direction.x,
                fabsf(direction.y) < 1.0e-20f ? FAR : 1 / direction.y,
                fabsf(direction.z) < 1.0e-20f ? FAR : 1 / direction.z);
        uint32_t stack[16], used = 1;
        stack[0] = 0;
        while (used) {
            Node *n = &nodes[stack[--used]];
            if (!intersects_box(n, origin, inverse, hit.distance)) {
                continue;
            }
            if (n->count) {
                for (uint32_t i = n->first; i < n->first + n->count; ++i) {
                    intersect_triangle(indices[i], origin, direction, &hit);
                }
            } else {
                int positive = component(direction, n->first) >= 0;
                stack[used++] = positive ? n->right : n->left;
                stack[used++] = positive ? n->left : n->right;
            }
        }
    }
    return hit;
}
int
trace_test(Vec o, Vec d, int linear, float *distance)
{
    Hit h = trace(o, d, FAR, linear);
    *distance = h.distance;
    return h.object;
}

static uint64_t
mix(uint64_t value)
{
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}
static float
random_float(uint64_t *state)
{
    *state += UINT64_C(0x9e3779b97f4a7c15);
    return (mix(*state) >> 40) * (1.0f / 16777216);
}
static Vec
camera_ray(uint32_t x, uint32_t y, float sx, float sy)
{
    float u = (2 * (x + sx) / scene.width - 1) *
              ((float)scene.width / scene.height) * half_height;
    float v = (1 - 2 * (y + sy) / scene.height) * half_height;
    return unit(
        add(forward, add(scale(camera_right, u), scale(camera_up, v))));
}
static Vec
path_sample(uint32_t x, uint32_t y, uint32_t sample, const Job *j)
{
    uint64_t state = mix(j->seed ^ mix((uint64_t)y * scene.width + x) ^
                         mix((uint64_t)sample + UINT64_C(0x100000000)));
    /* Explicit order avoids C argument evaluation order changing the RNG. */
    float sx = random_float(&state), sy = random_float(&state);
    Vec origin = scene.position, direction = camera_ray(x, y, sx, sy);
    Vec throughput = vec(1, 1, 1), result = vec(0, 0, 0);
    for (uint32_t depth = 0; depth <= j->depth; ++depth) {
        Hit hit = trace(origin, direction, FAR, 0);
        if (hit.object < 0) {
            break;
        }
        Material *m = &materials[triangles[hit.object].material];
        result = add(result, mul(throughput, m->emission));
        throughput = mul(throughput, m->albedo);
        if (depth == j->depth || !nonzero(throughput)) {
            break;
        }
        origin = add(add(origin, scale(direction, hit.distance)),
                     scale(hit.normal, EPS));
        float r = sqrtf(random_float(&state));
        float phi = 2 * PI * random_float(&state);
        Vec tangent = unit(
            cross(fabsf(hit.normal.x) > 0.9f ? vec(0, 1, 0) : vec(1, 0, 0),
                  hit.normal));
        Vec bitangent = cross(hit.normal, tangent);
        direction = add(add(scale(tangent, r * cosf(phi)),
                            scale(bitangent, r * sinf(phi))),
                        scale(hit.normal, sqrtf(max_float(0, 1 - r * r))));
        direction = unit(direction);
    }
    return result;
}
static Vec
smoke_sample(uint32_t x, uint32_t y)
{
    Vec direction = camera_ray(x, y, 0.5f, 0.5f);
    Hit hit = trace(scene.position, direction, FAR, 0);
    if (hit.object < 0) {
        return vec(0, 0, 0);
    }
    Material *m = &materials[triangles[hit.object].material];
    Vec result = m->emission;
    Vec point = add(scene.position, scale(direction, hit.distance));
    Vec origin = add(point, scale(hit.normal, EPS));
    for (uint32_t i = 0; i < light_count; ++i) {
        uint32_t id = lights[i];
        if (id == (uint32_t)hit.object) {
            continue;
        }
        Triangle *light = &triangles[id];
        Vec delta = sub(light->center, point);
        float squared = dot(delta, delta), distance = sqrtf(squared);
        if (distance <= EPS) {
            continue;
        }
        Vec d = scale(delta, 1 / distance);
        float cs = max_float(0, dot(hit.normal, d));
        float cl = fabsf(dot(light->normal, d));
        if (cs == 0 || cl == 0) {
            continue;
        }
        /* Visibility uses the offset origin, aimed exactly at the centroid. */
        Vec shadow = sub(light->center, origin);
        float shadow_length = sqrtf(dot(shadow, shadow));
        Hit blocker = trace(origin, scale(shadow, 1 / shadow_length),
                            shadow_length - 2 * EPS, 0);
        if (blocker.object >= 0) {
            continue;
        }
        Vec e = materials[light->material].emission;
        result = add(result, scale(mul(m->albedo, e),
                                   light->area * cs * cl / (PI * squared)));
    }
    return result;
}
static uint8_t
channel(float x)
{
    return (uint8_t)(255 * min_float(1, max_float(0, x)) + 0.5f);
}

int
render_local(const uint8_t *job_bytes, const uint8_t *scene_bytes,
             size_t scene_size, uint64_t job_addr,
             int (*write_row)(uint64_t, const uint8_t *, uint32_t))
{
    if (!job_bytes || (uintptr_t)job_bytes % 8) {
        return 9;
    }
    Job j;
    decode_job(job_bytes, &j);
#ifdef __riscv
    if (scene_size > SCENE_CAPACITY || j.scene_addr < 0x80000000 ||
        j.scene_addr > 0x84000000 - scene_size) {
        return 10;
    }
#endif
    int error = load_scene(scene_bytes, scene_size);
    if (error) {
        return error;
    }
    error = validate_job(&j);
    if (error) {
        return error;
    }
    uint64_t output_size =
        (uint64_t)(scene.height - 1) * j.stride + 4ULL * scene.width;
    if (j.output_addr < job_addr + 56 &&
        job_addr < j.output_addr + output_size) {
        return 11;
    }
#ifdef __riscv
    if (output_size > 0x4000000 || j.output_addr < 0x80000000 ||
        j.output_addr > 0x84000000 - output_size) {
        return 12;
    }
#endif
    for (uint32_t ty = 0; ty < j.height; ty += TILE_SIZE) {
        uint32_t h = j.height - ty < TILE_SIZE ? j.height - ty : TILE_SIZE;
        for (uint32_t tx = 0; tx < j.width; tx += TILE_SIZE) {
            uint32_t w = j.width - tx < TILE_SIZE ? j.width - tx : TILE_SIZE;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    Vec sum = vec(0, 0, 0);
                    if (j.mode == 1) {
                        sum = smoke_sample(j.x + tx + x, j.y + ty + y);
                    } else {
                        for (uint32_t s = 0; s < j.samples; ++s) {
                            sum = add(sum, path_sample(j.x + tx + x,
                                                       j.y + ty + y, s, &j));
                        }
                        sum = scale(sum, 1.0f / j.samples);
                    }
                    uint8_t *pixel = tile + 4 * (y * TILE_SIZE + x);
                    pixel[0] = channel(sum.x);
                    pixel[1] = channel(sum.y);
                    pixel[2] = channel(sum.z);
                    pixel[3] = 255;
                }
            }
            for (uint32_t y = 0; y < h; ++y) {
                uint8_t *output =
                    (uint8_t *)(uintptr_t)(j.output_addr +
                                           (uint64_t)(j.y + ty + y) *
                                               j.stride +
                                           4ULL * (j.x + tx));
                const uint8_t *row = tile + y * TILE_SIZE * 4;
                if (write_row) {
                    int error = write_row((uintptr_t)output, row, 4 * w);
                    if (error) {
                        return error;
                    }
                } else {
                    for (uint32_t b = 0; b < 4 * w; ++b) {
                        output[b] = row[b];
                    }
                }
            }
        }
    }
    return 0;
}

int
render(const uint8_t *job_bytes, size_t scene_size)
{
    if (!job_bytes || (uintptr_t)job_bytes % 8) {
        return 9;
    }
    Job job;
    decode_job(job_bytes, &job);
    return render_local(job_bytes, (const uint8_t *)(uintptr_t)job.scene_addr,
                        scene_size, (uintptr_t)job_bytes, NULL);
}
