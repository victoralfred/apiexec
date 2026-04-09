#include "core/cursor.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace apiexec;

void test_initial_state() {
    Cursor c;
    assert(c.time_window_start == 0);
    assert(c.time_window_end == 0);
    assert(c.page_token.empty());
    assert(c.exhausted == false);
    assert(c.extra.empty());
    assert(!c.has_time_window());
    std::cout << "  PASS: initial_state\n";
}

void test_time_window() {
    Cursor c;
    c.time_window_start = 1000;
    c.time_window_end = 5000;
    assert(c.has_time_window());
    std::cout << "  PASS: time_window\n";
}

void test_page_token_carry_forward() {
    Cursor c;
    c.page_token = "abc123";
    Cursor next = c;
    next.page_token = "def456";
    assert(next.page_token == "def456");
    assert(c.page_token == "abc123");
    std::cout << "  PASS: page_token_carry_forward\n";
}

void test_exhaustion() {
    Cursor c;
    assert(!c.exhausted);
    c.exhausted = true;
    assert(c.exhausted);
    std::cout << "  PASS: exhaustion\n";
}

void test_extra_map_basic() {
    Cursor c;
    assert(c.set_extra("key1", "value1"));
    assert(c.extra.size() == 1);
    assert(c.extra["key1"] == "value1");
    assert(c.set_extra("key1", "updated"));
    assert(c.extra["key1"] == "updated");
    assert(c.extra.size() == 1);
    std::cout << "  PASS: extra_map_basic\n";
}

void test_extra_map_max_entries() {
    Cursor c;
    for (std::size_t i = 0; i < kCursorExtraMaxEntries; ++i) {
        assert(c.set_extra("key" + std::to_string(i), "val"));
    }
    assert(c.extra.size() == kCursorExtraMaxEntries);
    assert(!c.set_extra("overflow", "val"));
    assert(c.extra.size() == kCursorExtraMaxEntries);
    assert(c.set_extra("key0", "new_val"));
    assert(c.extra["key0"] == "new_val");
    std::cout << "  PASS: extra_map_max_entries\n";
}

void test_extra_map_max_value_length() {
    Cursor c;
    std::string long_val(kCursorExtraMaxValueLen, 'x');
    assert(c.set_extra("key", long_val));
    std::string too_long(kCursorExtraMaxValueLen + 1, 'x');
    assert(!c.set_extra("key2", too_long));
    std::cout << "  PASS: extra_map_max_value_length\n";
}

void test_clear_extra() {
    Cursor c;
    c.set_extra("a", "1");
    c.set_extra("b", "2");
    assert(c.extra.size() == 2);
    c.clear_extra();
    assert(c.extra.empty());
    std::cout << "  PASS: clear_extra\n";
}

int main() {
    std::cout << "cursor_test:\n";
    test_initial_state();
    test_time_window();
    test_page_token_carry_forward();
    test_exhaustion();
    test_extra_map_basic();
    test_extra_map_max_entries();
    test_extra_map_max_value_length();
    test_clear_extra();
    std::cout << "All cursor tests passed.\n";
    return 0;
}
