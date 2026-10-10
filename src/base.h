// Basic types, asserts, small math and file helpers. Shared by every binary (unity builds).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)

#define FATAL(...) do { fprintf(stderr, "fatal: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define ASSERT(c) do { if (!(c)) FATAL("%s:%d: assert(%s)", __FILE__, __LINE__, #c); } while (0)

typedef struct { f32 x, y, z; } v3;

static inline v3  v3_make(f32 x, f32 y, f32 z) { v3 r = {x, y, z}; return r; }
static inline v3  v3_add(v3 a, v3 b) { return v3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3  v3_sub(v3 a, v3 b) { return v3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3  v3_scale(v3 a, f32 s) { return v3_make(a.x * s, a.y * s, a.z * s); }
static inline f32 v3_dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3  v3_cross(v3 a, v3 b) { return v3_make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline v3  v3_norm(v3 a) { f32 l = sqrtf(v3_dot(a, a)); return l > 0 ? v3_scale(a, 1.0f / l) : a; }

// Whole file into malloc'd memory; returns NULL on failure.
static void* read_file(const char* path, size_t* size_out)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
#ifdef _WIN32   // long is 32-bit on Windows
    _fseeki64(f, 0, SEEK_END);
    i64 size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    fseek(f, 0, SEEK_END);
    i64 size = ftell(f);
    fseek(f, 0, SEEK_SET);
#endif
    void* data = malloc(size > 0 ? (size_t)size : 1);
    if (size < 0 || fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); fclose(f); return NULL; }
    fclose(f);
    if (size_out) *size_out = (size_t)size;
    return data;
}
