/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibUnicode/Forward.h>
#include <LibWeb/DOM/ChildNode.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NonDocumentTypeChildNode.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

// The callbacks libweb_rust makes as it replaces the data of a CharacterData node (see Rust/src/dom/character_data.rs).
struct RustCharacterDataHooks {
    static void character_data_replaced(Node*, size_t old_data, size_t offset, size_t count, size_t data_length, u8 steps);
    static AbstractRange* selection_range(Node* document);
};

// https://dom.spec.whatwg.org/#characterdata
class WEB_API AK_COMPACT_EMPTY_BASES CharacterData
    : public Node
    , public ChildNode<CharacterData>
    , public NonDocumentTypeChildNode<CharacterData> {
    WEB_WRAPPABLE(CharacterData, Node);
    GC_DECLARE_ALLOCATOR(CharacterData);

public:
    virtual ~CharacterData() override;

    Utf16String const& data() const { return m_character_data_core.data; }
    void set_data(Utf16View const&);
    void set_data(Utf16String const&);

    unsigned length_in_utf16_code_units() const { return m_character_data_core.data.length_in_code_units(); }

    WebIDL::ExceptionOr<Utf16String> substring_data(size_t offset_in_utf16_code_units, size_t count_in_utf16_code_units) const;
    WebIDL::ExceptionOr<void> append_data(Utf16View const&);
    WebIDL::ExceptionOr<void> insert_data(size_t offset_in_utf16_code_units, Utf16View const&);
    WebIDL::ExceptionOr<void> delete_data(size_t offset_in_utf16_code_units, size_t count_in_utf16_code_units);
    WebIDL::ExceptionOr<void> replace_data(size_t offset_in_utf16_code_units, size_t count_in_utf16_code_units, Utf16View const&);

    Unicode::Segmenter& grapheme_segmenter() const;
    Unicode::Segmenter& line_segmenter() const;
    Unicode::Segmenter& word_segmenter() const;

protected:
    struct RareData : Node::RareData {
        AK_ALLOC_WITH_KMALLOC;

        virtual ~RareData() override;

        OwnPtr<Unicode::Segmenter> grapheme_segmenter;
        OwnPtr<Unicode::Segmenter> line_segmenter;
        OwnPtr<Unicode::Segmenter> word_segmenter;
    };

    CharacterData(Document&, NodeType, Utf16String);

    virtual OwnPtr<Node::RareData> create_rare_data() const override;
    RareData& ensure_character_data_rare_data() const;
    RareData* character_data_rare_data();
    RareData const* character_data_rare_data() const;

private:
    friend struct RustCharacterDataHooks;

    virtual size_t external_memory_size() const override;

    WebIDL::ExceptionOr<void> replace_data(size_t offset, size_t count, Utf16View const&, Utf16String const* data_string);
    void did_replace_data(Utf16String const& old_data, size_t offset, size_t count, size_t data_length);

    RustFFI::CharacterDataCore m_character_data_core;
};

}
