#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace gnfs::util {

/// SmallVector - 内联小容量的向量，避免小数据的堆分配
/// 当元素数量 <= InlineCapacity 时，数据存储在栈上
/// 超出时自动切换到堆存储
template <typename T, size_t InlineCapacity> class SmallVector {
    static_assert(InlineCapacity > 0, "InlineCapacity must be positive");
    static_assert(InlineCapacity <= std::numeric_limits<size_t>::max() / sizeof(T),
                  "InlineCapacity is too large for the inline storage size");
    // grow() 在异常路径无法回滚 move,要求 T 的 move 构造不抛。
    // 注意:noexcept 默认 move-ctor 包括基本类型、std::string、std::vector、unique_ptr 等。
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "SmallVector<T>: T must be nothrow move constructible "
                  "(grow() cannot roll back on partial move failure)");

public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;

    // 构造函数
    SmallVector() noexcept : size_(0), capacity_(InlineCapacity), heap_data_(nullptr) {}

    ~SmallVector() {
        clear();
        if (heap_data_) {
            std::allocator<T>{}.deallocate(heap_data_, capacity_);
        }
    }

    // Move 构造
    SmallVector(SmallVector&& other) noexcept
        : size_(0), capacity_(InlineCapacity), heap_data_(nullptr) {
        if (other.is_inline()) {
            // 从内联存储移动
            for (size_t i = 0; i < other.size_; ++i) {
                std::construct_at(inline_ptr() + i, std::move(other.inline_ptr()[i]));
            }
            size_ = other.size_;
            // 销毁源的 inline 元素（move 后仍需调析构函数）
            for (size_t i = 0; i < other.size_; ++i) {
                other.inline_ptr()[i].~T();
            }
        } else {
            // 直接接管堆存储
            heap_data_ = other.heap_data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.heap_data_ = nullptr;
        }
        other.size_ = 0;
        other.capacity_ = InlineCapacity;
    }

    // Move 赋值
    SmallVector& operator=(SmallVector&& other) noexcept {
        if (this != &other) {
            clear();
            if (heap_data_) {
                std::allocator<T>{}.deallocate(heap_data_, capacity_);
                heap_data_ = nullptr;
            }

            if (other.is_inline()) {
                for (size_t i = 0; i < other.size_; ++i) {
                    std::construct_at(inline_ptr() + i, std::move(other.inline_ptr()[i]));
                }
                size_ = other.size_;
                capacity_ = InlineCapacity;
                // 销毁源的 inline 元素（move 后仍需调析构函数）
                for (size_t i = 0; i < other.size_; ++i) {
                    other.inline_ptr()[i].~T();
                }
            } else {
                heap_data_ = other.heap_data_;
                size_ = other.size_;
                capacity_ = other.capacity_;
                other.heap_data_ = nullptr;
            }
            other.size_ = 0;
            other.capacity_ = InlineCapacity;
        }
        return *this;
    }

    // 禁止拷贝（性能考虑，需要时可手动实现）
    SmallVector(const SmallVector&) = delete;
    SmallVector& operator=(const SmallVector&) = delete;

    // 容量查询
    [[nodiscard]] size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] bool is_inline() const noexcept {
        return heap_data_ == nullptr;
    }
    [[nodiscard]] static constexpr size_t max_size() noexcept {
        return std::numeric_limits<size_t>::max() / sizeof(T);
    }

    // 元素访问（operator[] 在 Debug 模式下做边界检查）
    [[nodiscard]] T& operator[](size_t i) noexcept {
        assert(i < size_ && "SmallVector::operator[] index out of bounds");
        return data()[i];
    }
    [[nodiscard]] const T& operator[](size_t i) const noexcept {
        assert(i < size_ && "SmallVector::operator[] index out of bounds");
        return data()[i];
    }

    // 带异常的边界检查访问
    [[nodiscard]] T& at(size_t i) {
        if (i >= size_)
            throw std::out_of_range("SmallVector::at index out of bounds");
        return data()[i];
    }
    [[nodiscard]] const T& at(size_t i) const {
        if (i >= size_)
            throw std::out_of_range("SmallVector::at index out of bounds");
        return data()[i];
    }

    [[nodiscard]] T& front() noexcept {
        assert(size_ > 0 && "SmallVector::front on empty vector");
        return data()[0];
    }
    [[nodiscard]] const T& front() const noexcept {
        assert(size_ > 0 && "SmallVector::front on empty vector");
        return data()[0];
    }

    [[nodiscard]] T& back() noexcept {
        assert(size_ > 0 && "SmallVector::back on empty vector");
        return data()[size_ - 1];
    }
    [[nodiscard]] const T& back() const noexcept {
        assert(size_ > 0 && "SmallVector::back on empty vector");
        return data()[size_ - 1];
    }

    [[nodiscard]] T* data() noexcept {
        return heap_data_ ? heap_data_ : inline_ptr();
    }
    [[nodiscard]] const T* data() const noexcept {
        return heap_data_ ? heap_data_ : inline_ptr();
    }

    // 迭代器
    [[nodiscard]] iterator begin() noexcept {
        return data();
    }
    [[nodiscard]] iterator end() noexcept {
        return data() + size_;
    }
    [[nodiscard]] const_iterator begin() const noexcept {
        return data();
    }
    [[nodiscard]] const_iterator end() const noexcept {
        return data() + size_;
    }
    [[nodiscard]] const_iterator cbegin() const noexcept {
        return data();
    }
    [[nodiscard]] const_iterator cend() const noexcept {
        return data() + size_;
    }

    // 修改操作
    void push_back(const T& value) {
        emplace_back(value);
    }

    void push_back(T&& value) {
        emplace_back(std::move(value));
    }

    template <typename... Args> T& emplace_back(Args&&... args) {
        if (size_ == max_size()) {
            throw std::length_error("SmallVector size exceeds max_size");
        }
        append(size_ + 1, [&](T* ptr) { std::construct_at(ptr, std::forward<Args>(args)...); });
        return back();
    }

    void pop_back() noexcept {
        if (size_ > 0) {
            --size_;
            data()[size_].~T();
        }
    }

    void clear() noexcept {
        for (size_t i = 0; i < size_; ++i) {
            data()[i].~T();
        }
        size_ = 0;
    }

    void reserve(size_t new_cap) {
        if (new_cap > capacity_) {
            grow(new_cap);
        }
    }

    void resize(size_t new_size) {
        if (new_size > size_) {
            append(new_size, [](T* ptr) { std::construct_at(ptr); });
        } else {
            for (size_t i = new_size; i < size_; ++i) {
                data()[i].~T();
            }
        }
        size_ = new_size;
    }

    void resize(size_t new_size, const T& value) {
        if (new_size > size_) {
            append(new_size, [&](T* ptr) { std::construct_at(ptr, value); });
        } else {
            for (size_t i = new_size; i < size_; ++i) {
                data()[i].~T();
            }
        }
        size_ = new_size;
    }

private:
    size_t size_;
    size_t capacity_;
    T* heap_data_;
    alignas(T) unsigned char inline_storage_[sizeof(T) * InlineCapacity];

    T* inline_ptr() noexcept {
        return reinterpret_cast<T*>(inline_storage_);
    }

    const T* inline_ptr() const noexcept {
        return reinterpret_cast<const T*>(inline_storage_);
    }

    static void check_capacity(size_t capacity) {
        if (capacity > max_size()) {
            throw std::length_error("SmallVector capacity exceeds max_size");
        }
    }

    // T's move constructor cannot throw. New elements must already be
    // constructed before relocating values they may refer to.
    void relocate_to(T* new_data, size_t new_cap) noexcept {
        T* old_data = data();
        for (size_t i = 0; i < size_; ++i) {
            std::construct_at(new_data + i, std::move(old_data[i]));
            old_data[i].~T();
        }

        // 释放旧的堆存储
        if (heap_data_) {
            std::allocator<T>{}.deallocate(heap_data_, capacity_);
        }

        heap_data_ = new_data;
        capacity_ = new_cap;
    }

    void grow(size_t new_cap) {
        check_capacity(new_cap);
        relocate_to(std::allocator<T>{}.allocate(new_cap), new_cap);
    }

    template <typename Construct> void append(size_t new_size, Construct&& construct) {
        check_capacity(new_size);
        const bool reallocating = new_size > capacity_;
        size_t new_cap = capacity_;
        T* destination = data();
        if (reallocating) {
            new_cap = capacity_ > max_size() / 2 ? max_size() : capacity_ * 2;
            if (new_cap < new_size)
                new_cap = new_size;
            destination = std::allocator<T>{}.allocate(new_cap);
        }

        // Arguments may alias an element or its storage. Keep the original
        // elements alive until every new element has been constructed.
        size_t constructed = size_;
        try {
            for (; constructed < new_size; ++constructed)
                construct(destination + constructed);
        } catch (...) {
            while (constructed > size_)
                destination[--constructed].~T();
            if (reallocating)
                std::allocator<T>{}.deallocate(destination, new_cap);
            throw;
        }
        if (reallocating)
            relocate_to(destination, new_cap);
        size_ = new_size;
    }
};

} // namespace gnfs::util
