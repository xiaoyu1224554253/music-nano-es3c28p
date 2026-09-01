#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 跨任务共享变量的原子访问封装 (全部用 GCC __atomic 内置函数, 顺序一致模型).
 * 多任务间 (如 LVGL 读 / 采样任务写) 共享简单标量时, 用原子读写避免撕裂与缓存不一致.
 * __ATOMIC_SEQ_CST: 顺序一致语义, 所有任务看到一致的读写顺序. */

/* 原子读取 bool: p=目标变量地址 */
static inline bool atomic_load_bool(const volatile bool *p) {
    return __atomic_load_n((const bool *)p, __ATOMIC_SEQ_CST);
}

/* 原子写入 bool: p=目标变量地址, val=要写入的值 */
static inline void atomic_store_bool(volatile bool *p, bool val) {
    __atomic_store_n((bool *)p, val, __ATOMIC_SEQ_CST);
}

/* 原子读取 int32_t: p=目标变量地址 */
static inline int32_t atomic_load_i32(const volatile int32_t *p) {
    return __atomic_load_n((const int32_t *)p, __ATOMIC_SEQ_CST);
}

/* 原子写入 int32_t: p=目标变量地址, val=要写入的值 */
static inline void atomic_store_i32(volatile int32_t *p, int32_t val) {
    __atomic_store_n((int32_t *)p, val, __ATOMIC_SEQ_CST);
}

/* 原子读取 float: p=目标变量地址 (float 用双操作数重载, 通过局部变量交换) */
static inline float atomic_load_float(const volatile float *p) {
    float val;
    __atomic_load((const float *)p, &val, __ATOMIC_SEQ_CST);
    return val;
}

/* 原子写入 float: p=目标变量地址, val=要写入的值 */
static inline void atomic_store_float(volatile float *p, float val) {
    __atomic_store((float *)p, &val, __ATOMIC_SEQ_CST);
}
