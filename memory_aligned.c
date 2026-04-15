
#include "memory_aligned.h" 
#include <string.h>  /* for memcpy */
/**
 * @brief 一个保证内存对齐的 realloc 替代函数。
 * * @param ptr         指向现有内存块的指针；如果为 NULL，则行为类似 aligned_alloc。
 * @param old_size    旧内存块的大小（以字节为单位）。在 ptr 不为 NULL 时必须提供。
 * @param new_size    请求的新内存块的大小（以字节为单位）。如果为 0，则行为类似 free。
 * @param alignment   要求的对齐字节数（必须是2的幂，例如 8 或 16）。
 * @return            返回一个指向新分配的、已对齐的内存块的指针；如果失败则返回 NULL。
 */
void *realloc_aligned(void *ptr, size_t old_size, size_t new_size, size_t alignment) {
    // 情况 1: new_size 为 0，行为类似 free(ptr)
    if (new_size == 0) {
        if (ptr != NULL) {
            free(ptr);
        }
        return NULL;
    }

    // 计算为满足对齐要求而需要分配的实际大小
    // aligned_alloc 要求分配的总大小必须是对齐值的整数倍
    size_t size_to_alloc = (new_size + alignment - 1) / alignment * alignment;
    
    // 情况 2: ptr 为 NULL，行为类似 aligned_alloc(alignment, new_size)
    if (ptr == NULL) {
        return aligned_alloc(alignment, size_to_alloc);
    }

    // 情况 3: 真正的重新分配 (ptr 非空, new_size 非零)
    void *new_ptr = aligned_alloc(alignment, size_to_alloc);
    if (new_ptr == NULL) {
        // 如果分配失败，则返回 NULL，并且不释放旧的内存块（与 realloc 行为一致）
        return NULL;
    }

    // 确定需要拷贝多少数据：取旧大小和新大小中的较小者
    size_t bytes_to_copy = old_size < new_size ? old_size : new_size;
    if (bytes_to_copy > 0) {
        memcpy(new_ptr, ptr, bytes_to_copy);
    }
    
    // 释放旧的内存块
    free(ptr);

    return new_ptr;
}
