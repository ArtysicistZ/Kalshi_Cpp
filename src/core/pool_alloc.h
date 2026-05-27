#pragma once

#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <sys/mman.h>

namespace kalshi {

template <typename T, size_t Capacity>
class Pool {
private:
    char* base_;
    void* free_head_;
    static constexpr size_t block_size_ = sizeof(T);

public:
    static_assert(
        Capacity > 0,
        "fatal: Capacity must be positive!"
    );
    static_assert(
        sizeof(T) >= sizeof(void*),
        "fatal: type has smaller size than void*!"
    );

    Pool() {
        base_ = static_cast<char*>(mmap(
            nullptr,
            Capacity * block_size_,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0
        ));
        if (base_ == MAP_FAILED) {
            fprintf(stderr, "fatal: memory allocation failed!\n");
            abort();
        }
        free_head_ = base_;
        for (size_t i = 0; i < Capacity - 1; i++) {
            char* this_block = base_ + (i * block_size_);
            char* next_block = base_ + ((i + 1) * block_size_);
            *reinterpret_cast<void**>(this_block) = next_block;
        }
        char* last_block = base_ + (Capacity - 1) * block_size_;
        *reinterpret_cast<void**>(last_block) = nullptr;
    }
    ~Pool() { munmap(base_, Capacity * block_size_); }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    T* allocate() {
        if (free_head_ == nullptr) return nullptr;
        void* ret = free_head_;
        free_head_ = *reinterpret_cast<void**>(free_head_);
        return reinterpret_cast<T*>(ret);
    }

    void deallocate(T* ptr) {
        *reinterpret_cast<void**>(ptr) = free_head_;
        free_head_ = ptr;
    }

    bool full() const { return free_head_ == nullptr; }

    static constexpr size_t capacity() { return Capacity; }

};

}