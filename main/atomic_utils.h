#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool atomic_load_bool(const volatile bool *p) {
    return __atomic_load_n((const bool *)p, __ATOMIC_SEQ_CST);
}

static inline void atomic_store_bool(volatile bool *p, bool val) {
    __atomic_store_n((bool *)p, val, __ATOMIC_SEQ_CST);
}

static inline int32_t atomic_load_i32(const volatile int32_t *p) {
    return __atomic_load_n((const int32_t *)p, __ATOMIC_SEQ_CST);
}

static inline void atomic_store_i32(volatile int32_t *p, int32_t val) {
    __atomic_store_n((int32_t *)p, val, __ATOMIC_SEQ_CST);
}

static inline float atomic_load_float(const volatile float *p) {
    float val;
    __atomic_load((const float *)p, &val, __ATOMIC_SEQ_CST);
    return val;
}

static inline void atomic_store_float(volatile float *p, float val) {
    __atomic_store((float *)p, &val, __ATOMIC_SEQ_CST);
}
