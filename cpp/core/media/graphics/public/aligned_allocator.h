

#ifndef __ALIGNED_ALLOCATOR_H__
#define __ALIGNED_ALLOCATOR_H__

#include <memory>
#include <new>

template<typename T, std::size_t Alignment = 16>
struct aligned_allocator {
    using value_type = T;
    static constexpr std::size_t alignment = Alignment;

    template<typename U>
    struct rebind {
        using other = aligned_allocator<U, Alignment>;
    };

    aligned_allocator() = default;

    template<typename U>
    aligned_allocator(const aligned_allocator<U, Alignment>&) noexcept {}

    T* allocate(std::size_t n) {
        return static_cast<T*>(::operator new(n * sizeof(T),
                                              std::align_val_t{alignment}));
    }

    void deallocate(T* p, std::size_t) noexcept {
        ::operator delete(p, std::align_val_t{alignment});
    }
};

#endif // __ALIGNED_ALLOCATOR_H__


