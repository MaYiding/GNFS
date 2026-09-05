#include "gnfs/util/small_vector.hpp"
#include "support/test_check.hpp"

#include <cstddef>
#include <iostream>
#include <limits>
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

int main() {
    std::cout << "=== SmallVector Tests ===" << std::endl;

    test_basic_operations();
    test_inline_to_heap();
    test_move_operations();
    test_iteration();
    test_emplace_back();
    test_clear_and_resize();
    test_capacity_overflow_is_rejected();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
