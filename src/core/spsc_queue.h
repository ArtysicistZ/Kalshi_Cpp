template<typename T, size_t Capacity>
class SPSCQueue {
    T data_[Capacity];
    alignas(64) std::atomic<size_t> head_{0};
                size_t cache_tail_{0};
    alignas(64) std::atomic<size_t> tail_{0};
                size_t cache_head_{0};

    SPSCQueue() {
        static_assert(
            Capacity & (Capacity - 1) == 0,
            "Capacity must be power of 2!"
        );
    }
    ~SPSCQueue();

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    bool try_push(const T& item) {
        
    }

};