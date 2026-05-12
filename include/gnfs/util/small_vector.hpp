#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace gnfs::util {

/// SmallVector - 内联小容量的向量，避免小数据的堆分配
/// 当元素数量 <= InlineCapacity 时，数据存储在栈上
/// 超出时自动切换到堆存储
template <typename T, size_t InlineCapacity>
class SmallVector {
    static_assert(InlineCapacity > 0, "InlineCapacity must be positive");
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
            ::operator delete(heap_data_);
        }
    }

    // Move 构造
    SmallVector(SmallVector&& other) noexcept
        : size_(0), capacity_(InlineCapacity), heap_data_(nullptr) {
        if (other.is_inline()) {
            // 从内联存储移动
            for (size_t i = 0; i < other.size_; ++i) {
                new (inline_ptr() + i) T(std::move(other.inline_ptr()[i]));
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
                ::operator delete(heap_data_);
                heap_data_ = nullptr;
            }

            if (other.is_inline()) {
                for (size_t i = 0; i < other.size_; ++i) {
                    new (inline_ptr() + i) T(std::move(other.inline_ptr()[i]));
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
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool is_inline() const noexcept { return heap_data_ == nullptr; }

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
        if (i >= size_) throw std::out_of_range("SmallVector::at index out of bounds");
        return data()[i];
    }
    [[nodiscard]] const T& at(size_t i) const {
        if (i >= size_) throw std::out_of_range("SmallVector::at index out of bounds");
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
    [[nodiscard]] iterator begin() noexcept { return data(); }
    [[nodiscard]] iterator end() noexcept { return data() + size_; }
    [[nodiscard]] const_iterator begin() const noexcept { return data(); }
    [[nodiscard]] const_iterator end() const noexcept { return data() + size_; }
    [[nodiscard]] const_iterator cbegin() const noexcept { return data(); }
    [[nodiscard]] const_iterator cend() const noexcept { return data() + size_; }

    // 修改操作
    void push_back(const T& value) {
        ensure_capacity(size_ + 1);
        new (data() + size_) T(value);
        ++size_;
    }

    void push_back(T&& value) {
        ensure_capacity(size_ + 1);
        new (data() + size_) T(std::move(value));
        ++size_;
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        ensure_capacity(size_ + 1);
        T* ptr = new (data() + size_) T(std::forward<Args>(args)...);
        ++size_;
        return *ptr;
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
            ensure_capacity(new_size);
            for (size_t i = size_; i < new_size; ++i) {
                new (data() + i) T();
            }
        } else {
            for (size_t i = new_size; i < size_; ++i) {
                data()[i].~T();
            }
        }
        size_ = new_size;
    }

    void resize(size_t new_size, const T& value) {
        if (new_size > size_) {
            ensure_capacity(new_size);
            for (size_t i = size_; i < new_size; ++i) {
                new (data() + i) T(value);
            }
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

    void ensure_capacity(size_t required) {
        if (required > capacity_) {
            // 至少翻倍增长
            size_t new_cap = capacity_ * 2;
            if (new_cap < required) {
                new_cap = required;
            }
            grow(new_cap);
        }
    }

    void grow(size_t new_cap) {
        T* new_data = static_cast<T*>(::operator new(sizeof(T) * new_cap));

        // 移动旧元素
        T* old_data = data();
        for (size_t i = 0; i < size_; ++i) {
            new (new_data + i) T(std::move(old_data[i]));
            old_data[i].~T();
        }

        // 释放旧的堆存储
        if (heap_data_) {
            ::operator delete(heap_data_);
        }

        heap_data_ = new_data;
        capacity_ = new_cap;
    }
};

} // namespace gnfs::util
