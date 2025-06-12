/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/StringView.h>
#include <AK/Trie.h>

TEST_CASE(normal_behavior)
{
    Trie<char, ByteString> dictionary('/', "");
    constexpr StringView data[] { "test"sv, "example"sv, "foo"sv, "foobar"sv };
    constexpr size_t total_chars = 18; // root (1), 'test' (4), 'example' (7), 'foo' (3), 'foobar' (3, "foo" already stored).
    for (auto& view : data) {
        auto it = view.begin();
        MUST(dictionary.insert(it, view.end(), view, [](auto& parent, auto& it) -> Optional<ByteString> { return ByteString::formatted("{}{}", parent.metadata_value(), *it); }));
    }

    size_t i = 0;
    MUST(dictionary.for_each_node_in_tree_order([&](auto&) {
        ++i;
    }));
    EXPECT_EQ(i, total_chars);

    for (auto& view : data) {
        auto it = view.begin();
        auto& node = dictionary.traverse_until_last_accessible_node(it, view.end());
        EXPECT(it.is_end());
        EXPECT(node.metadata().has_value());
        EXPECT_EQ(view, node.metadata_value());
    }

    constexpr StringView test_data_with_prefix_in_dict[] { "testx"sv, "exampley"sv, "fooa"sv, "foobarb"sv, "fox"sv, "text"sv };
    for (auto& view : test_data_with_prefix_in_dict) {
        auto it = view.begin();
        auto& node = dictionary.traverse_until_last_accessible_node(it, view.end());
        EXPECT(!it.is_end());
        EXPECT(node.metadata().has_value());
        EXPECT(view.starts_with(node.metadata_value()));
    }
}

TEST_CASE(iterate)
{
    Trie<int> bunch_of_numbers { 0 };
    Array<int, 64> input;
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = i;

    MUST(bunch_of_numbers.insert(input.begin(), input.end()));

    // Iteration order is preorder (order between adjacent nodes is not defined, but parents come before children)
    // in this case, the tree is linear.
    size_t i = 0;
    bool is_root = true;
    MUST(bunch_of_numbers.for_each_node_in_tree_order([&](auto& node) {
        if (is_root) {
            is_root = false;
            return;
        }
        EXPECT_EQ(input[i], node.value());
        ++i;
    }));
}
