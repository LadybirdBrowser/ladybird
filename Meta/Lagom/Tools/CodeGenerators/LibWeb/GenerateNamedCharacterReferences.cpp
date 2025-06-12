/*
 * Copyright (c) 2024, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/Array.h>
#include <AK/FixedArray.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

ErrorOr<void> generate_header_file(Core::File& file);
ErrorOr<void> generate_implementation_file(JsonObject& named_character_reference_data, Core::File& file);

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the Entities header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Entities implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(json_path, "Path to the JSON file to read from", "json-path", 'j', "json-path");
    args_parser.parse(arguments);

    auto json = TRY(read_entire_file_as_json(json_path));
    VERIFY(json.is_object());
    auto named_character_reference_data = json.as_object();

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(*generated_header_file));
    TRY(generate_implementation_file(named_character_reference_data, *generated_implementation_file));

    return 0;
}

struct Codepoints {
    u32 first;
    u32 second;
};

inline static StringView get_second_codepoint_enum_name(u32 codepoint)
{
    switch (codepoint) {
    case 0x0338:
        return "CombiningLongSolidusOverlay"sv;
    case 0x20D2:
        return "CombiningLongVerticalLineOverlay"sv;
    case 0x200A:
        return "HairSpace"sv;
    case 0x0333:
        return "CombiningDoubleLowLine"sv;
    case 0x20E5:
        return "CombiningReverseSolidusOverlay"sv;
    case 0xFE00:
        return "VariationSelector1"sv;
    case 0x006A:
        return "LatinSmallLetterJ"sv;
    case 0x0331:
        return "CombiningMacronBelow"sv;
    default:
        return "None"sv;
    }
}

ErrorOr<void> generate_header_file(Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };
    generator.append(R"~~~(
#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>

namespace Web::HTML {

enum class NamedCharacterReferenceSecondCodepoint {
    None,
    CombiningLongSolidusOverlay, // U+0338
    CombiningLongVerticalLineOverlay, // U+20D2
    HairSpace, // U+200A
    CombiningDoubleLowLine, // U+0333
    CombiningReverseSolidusOverlay, // U+20E5
    VariationSelector1, // U+FE00
    LatinSmallLetterJ, // U+006A
    CombiningMacronBelow, // U+0331
};

inline Optional<u16> named_character_reference_second_codepoint_value(NamedCharacterReferenceSecondCodepoint codepoint)
{
    switch (codepoint) {
    case NamedCharacterReferenceSecondCodepoint::None:
        return {};
    case NamedCharacterReferenceSecondCodepoint::CombiningLongSolidusOverlay:
        return 0x0338;
    case NamedCharacterReferenceSecondCodepoint::CombiningLongVerticalLineOverlay:
        return 0x20D2;
    case NamedCharacterReferenceSecondCodepoint::HairSpace:
        return 0x200A;
    case NamedCharacterReferenceSecondCodepoint::CombiningDoubleLowLine:
        return 0x0333;
    case NamedCharacterReferenceSecondCodepoint::CombiningReverseSolidusOverlay:
        return 0x20E5;
    case NamedCharacterReferenceSecondCodepoint::VariationSelector1:
        return 0xFE00;
    case NamedCharacterReferenceSecondCodepoint::LatinSmallLetterJ:
        return 0x006A;
    case NamedCharacterReferenceSecondCodepoint::CombiningMacronBelow:
        return 0x0331;
    default:
        VERIFY_NOT_REACHED();
    }
}

// Note: The first codepoint could fit in 17 bits, and the second could fit in 4 (if unsigned).
// However, to get any benefit from minimizing the struct size, it would need to be accompanied by
// bit-packing the g_named_character_reference_codepoints_lookup array, and then either
// using 5 bits for the second field (since enum bitfields are signed), or using a 4-bit wide
// unsigned integer type.
struct NamedCharacterReferenceCodepoints {
    u32 first : 24; // Largest value is U+1D56B
    NamedCharacterReferenceSecondCodepoint second : 8;
};
static_assert(sizeof(NamedCharacterReferenceCodepoints) == 4);

u16 named_character_reference_child_index(u16 node_index);
bool named_character_reference_is_end_of_word(u16 node_index);
Optional<NamedCharacterReferenceCodepoints> named_character_reference_codepoints_from_unique_index(u16 unique_index);
Optional<u16> named_character_reference_find_sibling_and_update_unique_index(u16 first_child_index, u8 character, u16& unique_index);

} // namespace Web::HTML

)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

class Node final : public RefCounted<Node> {
private:
    struct NonnullRefPtrNodeTraits {
        static unsigned hash(NonnullRefPtr<Node> const& node)
        {
            u32 hash = 0;
            for (int i = 0; i < 128; i++) {
                hash ^= ptr_hash(node->m_children[i].ptr());
            }
            hash ^= int_hash(static_cast<u32>(node->m_is_terminal));
            return hash;
        }
        static bool equals(NonnullRefPtr<Node> const& a, NonnullRefPtr<Node> const& b)
        {
            if (a->m_is_terminal != b->m_is_terminal)
                return false;
            for (int i = 0; i < 128; i++) {
                if (a->m_children[i] != b->m_children[i])
                    return false;
            }
            return true;
        }
    };

public:
    static NonnullRefPtr<Node> create()
    {
        return adopt_ref(*new (nothrow) Node());
    }

    using NodeTableType = HashTable<NonnullRefPtr<Node>, NonnullRefPtrNodeTraits, false>;

    void calc_numbers()
    {
        m_number = static_cast<u16>(m_is_terminal);
        for (int i = 0; i < 128; i++) {
            if (m_children[i] == nullptr)
                continue;
            m_children[i]->calc_numbers();
            m_number += m_children[i]->m_number;
        }
    }

    u8 num_direct_children()
    {
        u8 num = 0;
        for (int i = 0; i < 128; i++) {
            if (m_children[i] != nullptr)
                num += 1;
        }
        return num;
    }

    Array<RefPtr<Node>, 128>& children() { return m_children; }

    void set_as_terminal() { m_is_terminal = true; }

    bool is_terminal() const { return m_is_terminal; }

    u16 number() const { return m_number; }

private:
    Node() = default;

    Array<RefPtr<Node>, 128> m_children { 0 };
    bool m_is_terminal { false };
    u16 m_number { 0 };
};

struct UncheckedNode {
    RefPtr<Node> parent;
    char character;
    RefPtr<Node> child;
};

class DafsaBuilder {
    AK_MAKE_NONCOPYABLE(DafsaBuilder);

public:
    using MappingType = HashMap<StringView, String>;

    DafsaBuilder()
        : m_root(Node::create())
    {
    }

    void insert(StringView str)
    {
        // Must be inserted in sorted order
        VERIFY(str > m_previous_word);

        size_t common_prefix_len = 0;
        for (size_t i = 0; i < min(str.length(), m_previous_word.length()); i++) {
            if (str[i] != m_previous_word[i])
                break;
            common_prefix_len++;
        }

        minimize(common_prefix_len);

        RefPtr<Node> node;
        if (m_unchecked_nodes.size() == 0)
            node = m_root;
        else
            node = m_unchecked_nodes.last().child;

        auto remaining = str.substring_view(common_prefix_len);
        for (char const c : remaining) {
            VERIFY(node->children().at(c) == nullptr);

            auto child = Node::create();
            node->children().at(c) = child;
            m_unchecked_nodes.append(UncheckedNode { node, c, child });
            node = child;
        }
        node->set_as_terminal();

        bool fits = str.copy_characters_to_buffer(m_previous_word_buf, sizeof(m_previous_word_buf));
        // It's guaranteed that m_previous_word_buf is large enough to hold the longest named character reference
        VERIFY(fits);
        m_previous_word = StringView(m_previous_word_buf, str.length());
    }

    void minimize(size_t down_to)
    {
        if (m_unchecked_nodes.size() == 0)
            return;
        while (m_unchecked_nodes.size() > down_to) {
            auto unchecked_node = m_unchecked_nodes.take_last();
            auto child = unchecked_node.child.release_nonnull();
            auto it = m_minimized_nodes.find(child);
            if (it != m_minimized_nodes.end()) {
                unchecked_node.parent->children().at(unchecked_node.character) = *it;
            } else {
                m_minimized_nodes.set(child);
            }
        }
    }

    void calc_numbers()
    {
        m_root->calc_numbers();
    }

    Optional<size_t> get_unique_index(StringView str)
    {
        size_t index = 0;
        Node* node = m_root.ptr();

        for (char const c : str) {
            if (node->children().at(c) == nullptr)
                return {};
            for (int sibling_c = 0; sibling_c < 128; sibling_c++) {
                if (node->children().at(sibling_c) == nullptr)
                    continue;
                if (sibling_c < c) {
                    index += node->children().at(sibling_c)->number();
                }
            }
            node = node->children().at(c);
            if (node->is_terminal())
                index += 1;
        }

        return index;
    }

    NonnullRefPtr<Node> root()
    {
        return m_root;
    }

private:
    NonnullRefPtr<Node> m_root;
    Node::NodeTableType m_minimized_nodes;
    Vector<UncheckedNode> m_unchecked_nodes;
    char m_previous_word_buf[64];
    StringView m_previous_word = { m_previous_word_buf, 0 };
};

static u16 write_children(NonnullRefPtr<Node> node, SourceGenerator& generator, Vector<NonnullRefPtr<Node>>& queue, HashMap<Node*, u16>& child_indexes, u16 first_available_index)
{
    auto current_available_index = first_available_index;
    auto num_children = node->num_direct_children();
    u16 child_i = 0;
    for (u8 c = 0; c < 128; c++) {
        if (node->children().at(c) == nullptr)
            continue;
        auto child = node->children().at(c).release_nonnull();
        auto is_last_child = child_i == num_children - 1;

        if (!child_indexes.contains(child.ptr())) {
            auto child_num_children = child->num_direct_children();
            if (child_num_children > 0) {
                child_indexes.set(child, current_available_index);
                current_available_index += child_num_children;
            }
            queue.append(child);
        }

        auto member_generator = generator.fork();
        member_generator.set("char", StringView(&c, 1));
        member_generator.set("number", String::number(child->number()));
        member_generator.set("end_of_word", MUST(String::formatted("{}", child->is_terminal())));
        member_generator.set("end_of_list", MUST(String::formatted("{}", is_last_child)));
        auto child_index = child_indexes.get(child).value_or(0);
        member_generator.set("child_index", String::number(child_index));
        member_generator.append(R"~~~(    { '@char@', @number@, @end_of_word@, @end_of_list@, @child_index@ },
)~~~");

        child_i++;
    }
    return current_available_index;
}

ErrorOr<void> generate_implementation_file(JsonObject& named_character_reference_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };
    DafsaBuilder dafsa_builder;

    named_character_reference_data.for_each_member([&](auto& key, auto&) {
        dafsa_builder.insert(key.bytes_as_string_view().substring_view(1));
    });
    dafsa_builder.minimize(0);
    dafsa_builder.calc_numbers();

    // As a sanity check, confirm that the minimal perfect hashing doesn't
    // have any collisions
    {
        HashTable<size_t> index_set;

        named_character_reference_data.for_each_member([&](auto& key, auto&) {
            auto index = dafsa_builder.get_unique_index(key.bytes_as_string_view().substring_view(1)).value();
            VERIFY(!index_set.contains(index));
            index_set.set(index);
        });
        VERIFY(named_character_reference_data.size() == index_set.size());
    }

    auto index_to_codepoints = MUST(FixedArray<Codepoints>::create(named_character_reference_data.size()));

    named_character_reference_data.for_each_member([&](auto& key, auto& value) {
        auto codepoints = value.as_object().get_array("codepoints"sv).value();
        auto unique_index = dafsa_builder.get_unique_index(key.bytes_as_string_view().substring_view(1)).value();
        auto array_index = unique_index - 1;
        u32 second_codepoint = 0;
        if (codepoints.size() == 2) {
            second_codepoint = codepoints[1].template as_integer<u32>();
        }
        index_to_codepoints[array_index] = Codepoints { codepoints[0].template as_integer<u32>(), second_codepoint };
    });

    generator.append(R"~~~(
#include <LibWeb/HTML/Parser/Entities.h>

namespace Web::HTML {

static NamedCharacterReferenceCodepoints g_named_character_reference_codepoints_lookup[] = {
)~~~");

    for (auto codepoints : index_to_codepoints) {
        auto member_generator = generator.fork();
        member_generator.set("first_codepoint", MUST(String::formatted("0x{:X}", codepoints.first)));
        member_generator.set("second_codepoint_name", get_second_codepoint_enum_name(codepoints.second));
        member_generator.append(R"~~~(    {@first_codepoint@, NamedCharacterReferenceSecondCodepoint::@second_codepoint_name@},
)~~~");
    }

    generator.append(R"~~~(};

struct DafsaNode {
    // The actual alphabet of characters used in the list of named character references only
    // includes 61 unique characters ('1'...'8', ';', 'a'...'z', 'A'...'Z'), but we have
    // bits to spare and encoding this as a `u8` allows us to avoid the need for converting
    // between an `enum(u6)` containing only the alphabet and the actual `u8` character value.
    u8 character;
    // Nodes are numbered with "an integer which gives the number of words that
    // would be accepted by the automaton starting from that state." This numbering
    // allows calculating "a one-to-one correspondence between the integers 1 to L
    // (L is the number of words accepted by the automaton) and the words themselves."
    //
    // Essentially, this allows us to have a minimal perfect hashing scheme such that
    // it's possible to store & lookup the codepoint transformations of each named character
    // reference using a separate array.
    //
    // Empirically, the largest number in our DAFSA is 168, so all number values fit in a u8.
    u8 number;
    // If true, this node is the end of a valid named character reference.
    // Note: This does not necessarily mean that this node does not have child nodes.
    bool end_of_word : 1;
    // If true, this node is the end of a sibling list.
    // If false, then (index + 1) will contain the next sibling.
    bool end_of_list : 1;
    // Index of the first child of this node.
    // There are 3872 nodes in our DAFSA, so all indexes could fit in a u12.
    u16 child_index : 14;
};
static_assert(sizeof(DafsaNode) == 4);

static DafsaNode g_named_character_reference_dafsa[] = {
    { 0, 0, false, true, 1 },
)~~~");

    Vector<NonnullRefPtr<Node>> queue;
    HashMap<Node*, u16> child_indexes;

    u16 first_available_index = dafsa_builder.root()->num_direct_children() + 1;

    NonnullRefPtr<Node> node = dafsa_builder.root();
    while (true) {
        first_available_index = write_children(node, generator, queue, child_indexes, first_available_index);

        if (queue.size() == 0)
            break;
        node = queue.take_first();
    }

    generator.append(R"~~~(};

u16 named_character_reference_child_index(u16 node_index) {
    return g_named_character_reference_dafsa[node_index].child_index;
}

bool named_character_reference_is_end_of_word(u16 node_index) {
    return g_named_character_reference_dafsa[node_index].end_of_word;
}

// Note: The unique index is 1-based.
Optional<NamedCharacterReferenceCodepoints> named_character_reference_codepoints_from_unique_index(u16 unique_index) {
    if (unique_index == 0) return {};
    return g_named_character_reference_codepoints_lookup[unique_index - 1];
}

// Search `first_child_index` and siblings of `first_child_index` for a node with the value `character`.
// If found, returns the index of the node within the `dafsa` array. Otherwise, returns `null`.
// Updates `unique_index` as the array is traversed
Optional<u16> named_character_reference_find_sibling_and_update_unique_index(u16 first_child_index, u8 character, u16& unique_index) {
    auto index = first_child_index;
    while (true) {
        if (g_named_character_reference_dafsa[index].character < character) {
            unique_index += g_named_character_reference_dafsa[index].number;
        }
        if (g_named_character_reference_dafsa[index].character == character) {
            if (g_named_character_reference_dafsa[index].end_of_word) unique_index++;
            return index;
        }
        if (g_named_character_reference_dafsa[index].end_of_list) return {};
        index += 1;
    }
    VERIFY_NOT_REACHED();
}

} // namespace Web::HTML
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}
