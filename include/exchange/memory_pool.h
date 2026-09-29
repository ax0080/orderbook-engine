#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

// Under AddressSanitizer, freed slots are poisoned so a use-after-free of a
// pooled object is reported even though the memory never goes back to malloc.
#if defined(__SANITIZE_ADDRESS__)
#define EXCHANGE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define EXCHANGE_ASAN 1
#endif
#endif

#ifdef EXCHANGE_ASAN
#include <sanitizer/asan_interface.h>
#define EXCHANGE_POISON(p, n)   ASAN_POISON_MEMORY_REGION(p, n)
#define EXCHANGE_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION(p, n)
#else
#define EXCHANGE_POISON(p, n)   ((void)0)
#define EXCHANGE_UNPOISON(p, n) ((void)0)
#endif

namespace exchange {

template <typename T, std::size_t BlockSize = 4096>
    requires (sizeof(T) >= sizeof(void*))
class MemoryPool {

    struct alignas(alignof(T)) Block {
        unsigned char data[sizeof(T) * BlockSize];
    };

public:
    MemoryPool() = default;
    ~MemoryPool() = default;

    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    template <typename... Args>
    T* allocate(Args&&... args) {
        void* slot;
        if (free_head_) {
            slot = free_head_;
            EXCHANGE_UNPOISON(slot, sizeof(T));
            void* next = nullptr;
            std::memcpy(&next, static_cast<unsigned char*>(free_head_), sizeof(void*));
            free_head_ = next;
        } else {
            if (cursor_ >= BlockSize) {
                blocks_.push_back(std::make_unique<Block>());
                cursor_ = 0;
            }
            slot = reinterpret_cast<T*>(blocks_.back()->data) + cursor_;
            ++cursor_;
        }
        return ::new (slot) T{std::forward<Args>(args)...};
    }

    void deallocate(T* ptr) noexcept {
        ptr->~T();
        void* raw = static_cast<void*>(ptr);
        std::memcpy(raw, &free_head_, sizeof(void*));
        free_head_ = raw;
        EXCHANGE_POISON(raw, sizeof(T));
    }

private:
    std::vector<std::unique_ptr<Block>> blocks_;
    std::size_t cursor_  = BlockSize;
    void*       free_head_ = nullptr;
};

} // namespace exchange
