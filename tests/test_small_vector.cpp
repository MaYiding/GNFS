#include "gnfs/util/small_vector.hpp"
#include "support/test_check.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace gnfs::util;

void test_basic_operations() {
    std::cout << "Testing basic operations..." << std::endl;

    SmallVector<int, 4> vec;
    GNFS_TEST_CHECK(vec.empty());
    GNFS_TEST_CHECK(vec.size() == 0);
    GNFS_TEST_CHECK(vec.is_inline());

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    GNFS_TEST_CHECK(vec.size() == 3);
    GNFS_TEST_CHECK(vec[0] == 1);
    GNFS_TEST_CHECK(vec[1] == 2);
    GNFS_TEST_CHECK(vec[2] == 3);
    GNFS_TEST_CHECK(vec.is_inline());

    std::cout << "  Basic operations: PASS" << std::endl;
}

void test_inline_to_heap() {
    std::cout << "Testing inline to heap transition..." << std::endl;

    SmallVector<int, 2> vec;
    vec.push_back(1);
    vec.push_back(2);
    GNFS_TEST_CHECK(vec.is_inline());

    // 超出内联容量，切换到堆
    vec.push_back(3);
    GNFS_TEST_CHECK(!vec.is_inline());
    GNFS_TEST_CHECK(vec.size() == 3);
    GNFS_TEST_CHECK(vec[0] == 1);
    GNFS_TEST_CHECK(vec[1] == 2);
    GNFS_TEST_CHECK(vec[2] == 3);

    std::cout << "  Inline to heap transition: PASS" << std::endl;
}

void test_move_operations() {
    std::cout << "Testing move operations..." << std::endl;

    // 内联移动
    SmallVector<int, 4> vec1;
    vec1.push_back(1);
    vec1.push_back(2);

    SmallVector<int, 4> vec2(std::move(vec1));
    GNFS_TEST_CHECK(vec2.size() == 2);
    GNFS_TEST_CHECK(vec2[0] == 1);
    GNFS_TEST_CHECK(vec2[1] == 2);
    GNFS_TEST_CHECK(vec1.size() == 0);

    // 堆移动
    SmallVector<int, 2> vec3;
    vec3.push_back(1);
    vec3.push_back(2);
    vec3.push_back(3); // 切换到堆

    SmallVector<int, 2> vec4(std::move(vec3));
    GNFS_TEST_CHECK(vec4.size() == 3);
    GNFS_TEST_CHECK(!vec4.is_inline());
    GNFS_TEST_CHECK(vec3.size() == 0);

    std::cout << "  Move operations: PASS" << std::endl;
}

void test_iteration() {
    std::cout << "Testing iteration..." << std::endl;

    SmallVector<int, 4> vec;
    vec.push_back(10);
    vec.push_back(20);
    vec.push_back(30);

    int sum = 0;
    for (int val : vec) {
        sum += val;
    }
    GNFS_TEST_CHECK(sum == 60);

    std::cout << "  Iteration: PASS" << std::endl;
}

void test_emplace_back() {
    std::cout << "Testing emplace_back..." << std::endl;

    SmallVector<std::string, 2> vec;
    vec.emplace_back("hello");
    vec.emplace_back("world");

    GNFS_TEST_CHECK(vec.size() == 2);
    GNFS_TEST_CHECK(vec[0] == "hello");
    GNFS_TEST_CHECK(vec[1] == "world");

    std::cout << "  emplace_back: PASS" << std::endl;
}

void test_clear_and_resize() {
    std::cout << "Testing clear and resize..." << std::endl;

    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    vec.clear();
    GNFS_TEST_CHECK(vec.empty());
    GNFS_TEST_CHECK(vec.size() == 0);

    vec.resize(5, 42);
    GNFS_TEST_CHECK(vec.size() == 5);
    for (std::size_t i = 0; i < vec.size(); ++i) {
        GNFS_TEST_CHECK(vec[i] == 42);
    }

    std::cout << "  Clear and resize: PASS" << std::endl;
}

void test_capacity_overflow_is_rejected() {
    std::cout << "Testing capacity overflow checks..." << std::endl;

    SmallVector<int, 2> vec;
    bool threw = false;
    try {
        vec.reserve(std::numeric_limits<std::size_t>::max());
    } catch (const std::length_error&) {
        threw = true;
    }
    GNFS_TEST_CHECK(threw);
    GNFS_TEST_CHECK(vec.empty());
    GNFS_TEST_CHECK(vec.is_inline());

    std::cout << "  Capacity overflow checks: PASS" << std::endl;
}

void test_capacity_overflow_guards() {
    std::cout << "Testing capacity overflow guards..." << std::endl;

    using Vector = SmallVector<int, 4>;
    constexpr std::size_t expected_max = std::numeric_limits<std::size_t>::max() / sizeof(int);
    static_assert(Vector::max_size() == expected_max);

    Vector vec;
    bool reserve_threw = false;
    try {
        vec.reserve(Vector::max_size() + 1);
    } catch (const std::length_error&) {
        reserve_threw = true;
    }
    GNFS_TEST_CHECK(reserve_threw);
    GNFS_TEST_CHECK(vec.empty());
    GNFS_TEST_CHECK(vec.capacity() == 4);

    bool resize_threw = false;
    try {
        vec.resize(Vector::max_size() + 1);
    } catch (const std::length_error&) {
        resize_threw = true;
    }
    GNFS_TEST_CHECK(resize_threw);
    GNFS_TEST_CHECK(vec.empty());

    std::cout << "  Capacity overflow guards: PASS" << std::endl;
}

void test_self_referencing_growth() {
    std::cout << "Testing self-referencing growth..." << std::endl;
    for (const std::size_t count : {2u, 4u}) {
        const std::string expected(128, 'x');
        auto make_full = [&] {
            SmallVector<std::string, 2> result;
            for (std::size_t i = 0; i < count; ++i)
                result.push_back(expected);
            GNFS_TEST_CHECK(result.size() == result.capacity());
            return result;
        };
        auto copied = make_full();
        copied.push_back(copied.front());
        GNFS_TEST_CHECK(copied.back() == expected);
        GNFS_TEST_CHECK(copied.front() == expected);

        auto moved = make_full();
        moved.push_back(std::move(moved.front()));
        GNFS_TEST_CHECK(moved.back() == expected);

        auto emplaced = make_full();
        emplaced.emplace_back(emplaced.front());
        GNFS_TEST_CHECK(emplaced.back() == expected);
        GNFS_TEST_CHECK(emplaced.front() == expected);

        auto subobject = make_full();
        subobject.emplace_back(subobject.front().data() + 1, subobject.front().size() - 1);
        GNFS_TEST_CHECK(subobject.back() == expected.substr(1));
        GNFS_TEST_CHECK(subobject.front() == expected);

        auto resized = make_full();
        resized.resize(count + 3, resized.front());
        GNFS_TEST_CHECK(resized.size() == count + 3);
        for (const auto& value : resized)
            GNFS_TEST_CHECK(value == expected);
    }
    SmallVector<std::unique_ptr<int>, 1> owned;
    owned.emplace_back(std::make_unique<int>(42));
    owned.push_back(std::move(owned.front()));
    GNFS_TEST_CHECK(!owned.front());
    GNFS_TEST_CHECK(*owned.back() == 42);
    std::cout << "  Self-referencing growth: PASS" << std::endl;
}

struct ThrowingElement {
    inline static int live = 0;
    inline static int successful_constructions_left = -1;
    int value = 7;

    static void check_construction() {
        if (successful_constructions_left == 0)
            throw std::runtime_error("injected element construction failure");
        if (successful_constructions_left > 0)
            --successful_constructions_left;
        ++live;
    }
    ThrowingElement() {
        check_construction();
    }
    ThrowingElement(const ThrowingElement& other) : value(other.value) {
        check_construction();
    }
    ThrowingElement(ThrowingElement&& other) noexcept : value(other.value) {
        ++live;
        other.value = -1;
    }
    ~ThrowingElement() {
        --live;
    }
};

void test_failed_construction_rollback() {
    std::cout << "Testing failed construction rollback..." << std::endl;
    for (const std::size_t initial_size : {2u, 4u}) {
        for (const bool spare_capacity : {false, true}) {
            for (const bool copy_fill : {false, true}) {
                for (const int successes : {0, 1, 2}) {
                    GNFS_TEST_CHECK(ThrowingElement::live == 0);
                    {
                        SmallVector<ThrowingElement, 2> values;
                        values.resize(initial_size);
                        values.front().value = 42;
                        if (spare_capacity)
                            values.reserve(8);
                        const auto* original_data = values.data();
                        const auto original_capacity = values.capacity();
                        ThrowingElement::successful_constructions_left = successes;
                        bool threw = false;
                        try {
                            if (copy_fill)
                                values.resize(initial_size + 3, values.front());
                            else
                                values.resize(initial_size + 3);
                        } catch (const std::runtime_error&) {
                            threw = true;
                        }
                        ThrowingElement::successful_constructions_left = -1;
                        GNFS_TEST_CHECK(threw);
                        GNFS_TEST_CHECK(values.size() == initial_size);
                        GNFS_TEST_CHECK(values.capacity() == original_capacity);
                        GNFS_TEST_CHECK(values.data() == original_data);
                        GNFS_TEST_CHECK(values.front().value == 42);
                        GNFS_TEST_CHECK(values.back().value == 7);
                        GNFS_TEST_CHECK(ThrowingElement::live == static_cast<int>(initial_size));
                        values.emplace_back();
                        GNFS_TEST_CHECK(values.size() == initial_size + 1);
                    }
                    GNFS_TEST_CHECK(ThrowingElement::live == 0);
                }
            }
        }
    }
    {
        SmallVector<ThrowingElement, 1> values;
        values.emplace_back();
        const auto* original_data = values.data();
        ThrowingElement::successful_constructions_left = 0;
        bool threw = false;
        try {
            values.emplace_back(values.front());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        ThrowingElement::successful_constructions_left = -1;
        GNFS_TEST_CHECK(threw);
        GNFS_TEST_CHECK(values.data() == original_data);
        GNFS_TEST_CHECK(values.size() == 1);
        GNFS_TEST_CHECK(values.front().value == 7);
        GNFS_TEST_CHECK(ThrowingElement::live == 1);
    }
    GNFS_TEST_CHECK(ThrowingElement::live == 0);
    std::cout << "  Failed construction rollback: PASS" << std::endl;
}

struct alignas(128) AlignedElement {
    int value = 0;
};

void test_overaligned_storage() {
    std::cout << "Testing over-aligned storage..." << std::endl;
    auto check_alignment = [](const auto& values) {
        GNFS_TEST_CHECK(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(AlignedElement) ==
                        0);
        for (std::size_t i = 0; i < values.size(); ++i)
            GNFS_TEST_CHECK(values[i].value == static_cast<int>(i));
    };
    SmallVector<AlignedElement, 2> values;
    check_alignment(values);
    for (int i = 0; i < 9; ++i) {
        values.emplace_back(AlignedElement{i});
        check_alignment(values);
    }
    values.reserve(40);
    check_alignment(values);
    auto moved = std::move(values);
    check_alignment(moved);
    SmallVector<AlignedElement, 2> assigned;
    assigned.resize(4);
    assigned = std::move(moved);
    check_alignment(assigned);
    values.emplace_back(AlignedElement{0});
    check_alignment(values);
    std::cout << "  Over-aligned storage: PASS" << std::endl;
}

int main() {
    test_self_referencing_growth();
    test_failed_construction_rollback();
    test_overaligned_storage();
    std::cout << "=== SmallVector Tests ===" << std::endl;

    test_basic_operations();
    test_inline_to_heap();
    test_move_operations();
    test_iteration();
    test_emplace_back();
    test_clear_and_resize();
    test_capacity_overflow_is_rejected();
    test_capacity_overflow_guards();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
