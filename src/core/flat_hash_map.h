#pragma once

#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <concepts>
#include <utility>
#include <sys/mman.h>

namespace kalshi {

template<typename Key, typename Value, size_t Capacity>
class FlatHashMap {
private:
    struct Slot {
        Key key;
        Value value;
    };
    Slot* slots_;
    size_t size_;
    static constexpr size_t block_size_ = sizeof(Slot);
    static constexpr size_t mask_ = Capacity - 1;
    static constexpr Key EMPTY = static_cast<Key>(-1);

public:
    static_assert(
        (Capacity & (Capacity - 1)) == 0,
        "fatal: Capacity must be power of 2!"
    );
    static_assert(
        Capacity > 0,
        "fatal: Capacity must be positive!"
    );

    FlatHashMap() {
        slots_ = static_cast<Slot*>(mmap(
            nullptr,
            Capacity * block_size_,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0
        ));
        if (slots_ == MAP_FAILED) {
            fprintf(stderr, "fatal: memory allocation failed!\n");
            abort();
        }
        for (size_t i = 0; i < Capacity; i++) {
            slots_[i].key = EMPTY;
        }
        size_ = 0;
    }
    ~FlatHashMap() { munmap(slots_, Capacity * block_size_); }

    FlatHashMap(const FlatHashMap&) = delete;
    FlatHashMap& operator=(const FlatHashMap&) = delete;

    template<typename V>
        requires std::assignable_from<Value&, V>
    bool insert(Key key, V&& value) {
        size_t pos = hash_(key) & mask_;
        size_t ctr = 0;
        while (slots_[pos].key != EMPTY && slots_[pos].key != key) {
            if (++ctr == Capacity) return false;
            pos = (pos + 1) & mask_;
        }
        bool inserting_new = (slots_[pos].key == EMPTY);
        slots_[pos].key = key;
        slots_[pos].value = std::forward<V>(value);
        if (inserting_new) size_++;
        return true;
    }

    Value* find(Key key) {
        size_t pos = hash_(key) & mask_;
        size_t ctr = 0;
        while (slots_[pos].key != key) {
            if (slots_[pos].key == EMPTY || ctr == Capacity) return nullptr;
            pos = (pos + 1) & mask_;
            ctr++;
        }
        return &slots_[pos].value;
    }

    bool erase(Key key) {
        size_t pos = hash_(key) & mask_;
        size_t ctr = 0;
        while (slots_[pos].key != key) {
            if (slots_[pos].key == EMPTY || ctr == Capacity) return false;
            pos = (pos + 1) & mask_;
            ctr++;
        }
        slots_[pos].key = EMPTY;
        size_--;

        size_t next = (pos + 1) & mask_;
        while (slots_[next].key != EMPTY) {
            if (probe_distance_(slots_[next].key, next) == 0) break;
            slots_[pos] = slots_[next];     // efficient?
            slots_[next].key = EMPTY;
            pos = next;
            next = (next + 1) & mask_;
        }
        return true;
    }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    static constexpr size_t capacity() { return Capacity; }

private:
    static size_t hash_(Key key) {
        x = static_cast<size_t>(key);
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    static size_t probe_distance_(Key key, size_t pos) {
        return (pos - hash_(key)) & mask_;
    }

};

}