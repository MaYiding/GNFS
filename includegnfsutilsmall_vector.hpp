#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace gnfs::util {

/// Small vector optimization: stores small arrays inline
template<typename T, size_t N = 8>
class SmallVector {
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;

    // Constructors
    SmallVector() : size_(0), capacity_(N), data_(inline_data()) {}

    explicit SmallVector(size_t count, const T& value = T()) : SmallVector() {
        resize(count, value);
    }

    SmallVector(std::initializer_list<T> init) : SmallVector() {
        reserve(init.size());
        for (const auto& val : init) {
            push_back(val);
        }
    }

    SmallVector(const SmallVector& other) : SmallVector() {
        reserve(other.size_);
        for (size_t i = 0; i < other.size_; ++i) {
            push_back(other.data_[i]);
        }
    }

    SmallVector(SmallVector&& other) noexcept : SmallVector() {
        if (other.is_inline()) {
            // Move inline elements
            for (size_t i = 0; i < other.size_; ++i) {
                push_back(std::move(other.data_[i]));
            }
        } else {
            // Steal heap allocation
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = other.inline_data();
            other.size_ = 0;
            other.capacity_ = N;
        }
    }

    ~SmallVector() {
        clear();
        if (!is_inline()) {
            ::operator delete(data_);
        }
    }

    // Assignment
    SmallVector& operator=(const SmallVector& other) {
        if (this != &other) {
            clear();
            reserve(other.size_);
            for (size_t i = 0; i < other.size_; ++i) {
                push_back(other.data_[i]);
            }
        }
        return *this;
    }

    SmallVector& operator=(SmallVector&& other) noexcept {
        if (this != &other) {
            clear();
            if (!is_inline()) {
                ::operator delete(data_);
                data_ = inline_data();
                capacity_ = N;
            }

            if (other.is_inline()) {
                for (size_t i = 0; i < other.size_; ++i) {
                    push_back(std::move(other.data_[i]));
                }
            } else {
                data_ = other.data_;
                size_ = other.size_;
                capacity_ = other.capacity_;
                other.data_ = other.inline_data();
                other.size_ = 0;
                other.capacity_ = N;
            }
        }
        return *this;
    }

    // Element access
    reference operator[](size_t pos) { return data_[pos]; }
    const_reference operator[](size_t pos) const { return data_[pos]; }

    reference at(size_t pos) {
        if (pos >= size_) throw std::out_of_range("SmallVector::at");
        return data_[pos];
    }

    const_reference at(size_t pos) const {
        if (pos >= size_) throw std::out_of_range("SmallVector::at");
        return data_[pos];
    }

    reference front() { return data_[0]; }
    const_reference front() const { return data_[0]; }
    reference back() { return data_[size_ - 1]; }
    const_reference back() const { return data_[size_ - 1]; }

    T* data() noexcept { return data_; }
    const T* data() const noexcept { return data_; }

    // Iterators
    iterator begin() noexcept { return data_; }
    const_iterator begin() const noexcept { return data_; }
    const_iterator cbegin() const noexcept { return data_; }
    iterator end() noexcept { return data_ + size_; }
    const_iterator end() const noexcept { return data_ + size_; }
    const_iterator cend() const noexcept { return data_ + size_; }

    // Capacity
    bool empty() const noexcept { return size_ == 0; }
    size_t size() const noexcept { return size_; }
    size_t capacity() const noexcept { return capacity_; }

    void reserve(size_t new_cap) {
        if (new_cap <= capacity_) return;

        T* new_data = static_cast<T*>(::operator new(new_cap * sizeof(T)));
        
        // Move existing elements
        for (size_t i = 0; i < size_; ++i) {
            new (&new_data[i]) T(std::move(data_[i]));
            data_[i].~T();
        }

        if (!is_inline()) {
            ::operator delete(data_);
        }

        data_ = new_data;
        capacity_ = new_cap;
    }

    void shrink_to_fit() {
        if (size_ <= N && !is_inline()) {
            T* new_data = inline_data();
            for (size_t i = 0; i < size_; ++i) {
                new (&new_data[i]) T(std::move(data_[i]));
                data_[i].~T();
            }
            ::operator delete(data_);
            data_ = new_data;
            capacity_ = N;
        }
    }

    // Modifiers
    void clear() noexcept {
        for (size_t i = 0; i < size_; ++i) {
            data_[i].~T();
        }
        size_ = 0;
    }

    void push_back(const T& value) {
        if (size_ >= capacity_) {
            reserve(capacity_ * 2);
        }
        new (&data_[size_]) T(value);
        ++size_;
    }

    void push_back(T&& value) {
        if (size_ >= capacity_) {
            reserve(capacity_ * 2);
        }
        new (&data_[size_]) T(std::move(value));
        ++size_;
    }

    template<typename... Args>
    void emplace_back(Args&&... args) {
        if (size_ >= capacity_) {
            reserve(capacity_ * 2);
        }
        new (&data_[size_]) T(std::forward<Args>(args)...);
        ++size_;
    }

    void pop_back() {
        if (size_ > 0) {
            data_[--size_].~T();
        }
    }

    void resize(size_t count, const T& value = T()) {
        if (count > size_) {
            reserve(count);
            for (size_t i = size_; i < count; ++i) {
                new (&data_[i]) T(value);
            }
        } else {
            for (size_t i = count; i < size_; ++i) {
                data_[i].~T();
            }
        }
        size_ = count;
    }

private:
    size_t size_;
    size_t capacity_;
    T* data_;
    alignas(T) char inline_storage_[N * sizeof(T)];

    T* inline_data() noexcept {
        return reinterpret_cast<T*>(inline_storage_);
    }

    const T* inline_data() const noexcept {
        return reinterpret_cast<const T*>(inline_storage_);
    }

    bool is_inline() const noexcept {
        return data_ == inline_data();
    }
};

} // namespace gnfs::util
