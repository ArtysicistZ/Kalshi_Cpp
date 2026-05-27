#pragma once

#include <iostream>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <memory_resource>
#include <new>
#include <sys/mman.h>

namespace kalshi {

class Arena : public std::pmr::memory_resource {
private:
    char* base_;
    size_t capacity_;
    size_t offset_;

public:
    explicit Arena(size_t bytes) {
        capacity_ = bytes;
        offset_ = 0;
        base_ = (char*)mmap(
            nullptr,
            bytes,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0
        );
        if (base_ == MAP_FAILED) {
            std::cerr << "fatal: memory allocation failed!\n";
            abort();
        }
    };
    ~Arena() { munmap(base_, capacity_); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* allocate(size_t bytes, size_t align = alignof(std::max_align_t)) {
        assert((align & (align - 1)) == 0);
        size_t aligned = (offset_ + align - 1) & ~(align - 1);
        if (aligned + bytes >= capacity_) return nullptr;
        offset_ = aligned + bytes;
        return base_ + aligned;
    }

    void reset() {
        offset_ = 0;
    }

    size_t used() const {
        return offset_;
    }

    size_t capacity() const {
        return capacity_;
    }

private:
    void* do_allocate(size_t bytes, size_t align) override {
        return allocate(bytes, align);
    }
    
    void do_deallocate(void* p, size_t bytes, size_t align) override {}

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

};

}