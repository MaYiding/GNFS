#include "gnfs/util/small_vector.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace gnfs::util;

void test_basic_operations() {
    std::cout << "Testing basic operations..." << std::endl;

    SmallVector<int, 4> vec;
    assert(vec.empty());
    assert(vec.size() == 0);
    assert(vec.is_inline());

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    assert(vec.size() == 3);
    assert(vec[0] == 1);
    assert(vec[1] == 2);
    assert(vec[2] == 3);
    assert(vec.is_inline());

    std::cout << "  Basic operations: PASS" << std::endl;
}

void test_inline_to_heap() {
    std::cout << "Testing inline to heap transition..." << std::endl;

    SmallVector<int, 2> vec;
    vec.push_back(1);
    vec.push_back(2);
    assert(vec.is_inline());

    // 超出内联容量，切换到堆
    vec.push_back(3);
    assert(!vec.is_inline());
    assert(vec.size() == 3);
    assert(vec[0] == 1);
    assert(vec[1] == 2);
    assert(vec[2] == 3);

    std::cout << "  Inline to heap transition: PASS" << std::endl;
}

void test_move_operations() {
    std::cout << "Testing move operations..." << std::endl;

    // 内联移动
    SmallVector<int, 4> vec1;
    vec1.push_back(1);
    vec1.push_back(2);

    SmallVector<int, 4> vec2(std::move(vec1));
    assert(vec2.size() == 2);
    assert(vec2[0] == 1);
    assert(vec2[1] == 2);
    assert(vec1.size() == 0);

    // 堆移动
    SmallVector<int, 2> vec3;
    vec3.push_back(1);
    vec3.push_back(2);
    vec3.push_back(3);  // 切换到堆

    SmallVector<int, 2> vec4(std::move(vec3));
    assert(vec4.size() == 3);
    assert(!vec4.is_inline());
    assert(vec3.size() == 0);

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
    assert(sum == 60);

    std::cout << "  Iteration: PASS" << std::endl;
}

void test_emplace_back() {
    std::cout << "Testing emplace_back..." << std::endl;

    SmallVector<std::string, 2> vec;
    vec.emplace_back("hello");
    vec.emplace_back("world");

    assert(vec.size() == 2);
    assert(vec[0] == "hello");
    assert(vec[1] == "world");

    std::cout << "  emplace_back: PASS" << std::endl;
}

void test_clear_and_resize() {
    std::cout << "Testing clear and resize..." << std::endl;

    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    vec.clear();
    assert(vec.empty());
    assert(vec.size() == 0);

    vec.resize(5, 42);
    assert(vec.size() == 5);
    for (size_t i = 0; i < vec.size(); ++i) {
        assert(vec[i] == 42);
    }

    std::cout << "  Clear and resize: PASS" << std::endl;
}

int main() {
    std::cout << "=== SmallVector Tests ===" << std::endl;

    test_basic_operations();
    test_inline_to_heap();
    test_move_operations();
    test_iteration();
    test_emplace_back();
    test_clear_and_resize();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
