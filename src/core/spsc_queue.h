template<typename T, size_t Capacity>
class SPSCQueue {
    T data_[Capacity];
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;

};