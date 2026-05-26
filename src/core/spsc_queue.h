#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <utility>

namespace kalshi {
    
template<typename T, size_t Capacity>
class SPSCQueue {

    static_assert(
        Capacity & (Capacity - 1) == 0,
        "Capacity must be power of 2!"
    );
    static_assert(
        Capacity > 0,
        "Capacity must be positive!"
    );

    T data_[Capacity];
    alignas(64) std::atomic<size_t> head_{0};
                size_t cached_tail_{0};
    alignas(64) std::atomic<size_t> tail_{0};
                size_t cached_head_{0};
    static constexpr size_t mask_ = Capacity - 1;

public: 
    SPSCQueue() = default;
    ~SPSCQueue() = default;
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    bool try_push(const T& item) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h - cached_tail_ == Capacity) {
            cached_tail_ = tail_.load();
            if (h - cached_tail_ == Capacity) 
                return false;
        }
        data_[h & mask_] = item;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool try_push(T&& item) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h - cached_tail_ == Capacity) {
            cached_tail_ = tail_.load();
            if (h - cached_tail_ == Capacity) 
                return false;
        }
        data_[h & mask_] = std::move(item);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    template <typename... Args>
    bool try_emplace(Args&&... args) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h - cached_tail_ == Capacity) {
            cached_tail_ = tail_.load();
            if (h - cached_tail_ == Capacity) 
                return false;
        }
        new (&data_[h & mask_]) T(std::forward<Args>(args)...);
        head_.store(h + 1, std::memory_order_release);
    }

    bool try_pop(T& item) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (cached_head_ - t == 0) {
            cached_head_ = head_.load();
            if (cached_head_ - t == 0)
                return false;
        }
        item = std::move(data_[t & mask_]);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    T* front() {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (cached_head_ - t == 0) {
            cached_head_ = head_.load();
            if (cached_head_ - t == 0)
                return nullptr;
        }
        T item = data_[t & mask_];
        return &item;
    }

    bool pop() {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (cached_head_ - t == 0) {
            cached_head_ = head_.load();
            if (cached_head_ - t == 0)
                return false;
        }
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    int size() {
        return head_.load(std::memory_order_relaxed)
             - tail_.load(std::memory_order_relaxed);
    }

    bool empty() {
        return size() == 0;
    }

    static constexpr size_t capacity() {
        return Capacity;
    }

};

}

