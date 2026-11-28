/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/SpellingMarkers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::DOM {

#if ARCH(X86_64) && !defined(AK_OS_WINDOWS)
static_assert(sizeof(Text) == 128);
#endif

struct Text::RareData final
    : CharacterData::RareData
    , SlottableMixin::RareData {
    AK_ALLOC_WITH_KMALLOC;

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        CharacterData::RareData::visit_edges(visitor);
        SlottableMixin::RareData::visit_edges(visitor);
    }
};

OwnPtr<Node::RareData> Text::create_rare_data() const
{
    return make<RareData>();
}

SlottableMixin::RareData* Text::slottable_rare_data()
{
    return static_cast<RareData*>(rare_data());
}

SlottableMixin::RareData const* Text::slottable_rare_data() const
{
    return static_cast<RareData const*>(rare_data());
}

SlottableMixin::RareData& Text::ensure_slottable_rare_data()
{
    return static_cast<RareData&>(ensure_rare_data());
}

static_assert(is_shell_of_rust_object<Text, RustFFI::TextObject>);

Text::Text(VtableProbe probe)
    : CharacterData(probe)
{
    // NB: libweb_rust reads a Text node as the TextPrefix it starts with.
    static_assert(sizeof(Text) == sizeof(RustFFI::TextPrefix));
    static_assert(offsetof(Text, m_text_core) == offsetof(RustFFI::TextPrefix, core));

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(Text, m_text_core) >= sizeof(CharacterData));
}

GC::Ref<Text> Text::create(Document& document, Utf16String data)
{
    return static_cast<Text&>(*RustFFI::rust_dom_create_text(&document, move(data).into_raw()));
}

GC::Ref<Text> Text::create_for_constructor(JS::Object& relevant_global_object, Utf16String data)
{
    return create(HTML::relevant_window(relevant_global_object).associated_document(), move(data));
}

// Spelling markers belong to the document whose spellchecker flagged the text, so they stay behind when the text moves
// to another document.
void Text::adopted_from(Document& old_document)
{
    Base::adopted_from(old_document);
    if (has_spelling_markers())
        old_document.spelling_markers().remove_all(*this);
}

// https://dom.spec.whatwg.org/#dom-text-splittext
// https://dom.spec.whatwg.org/#concept-text-split
WebIDL::ExceptionOr<GC::Ref<Text>> Text::split_text(size_t offset)
{
    // 1. Let length be node’s length.
    auto length = this->length();

    // 2. If offset is greater than length, then throw an "IndexSizeError" DOMException.
    if (offset > length)
        return WebIDL::IndexSizeError::create("Split offset is greater than length"_utf16);

    // 3. Let count be length minus offset.
    // 4. Let new data be the result of substringing data with node node, offset offset, and count count.
    // 5. Let new node be a new Text node, with the same node document as node. Set new node’s data to new data.
    // 6. Let parent be node’s parent.
    // 7. If parent is not null, then:
    // 8. Replace data with node node, offset offset, count count, and data the empty string.
    // 9. Return new node.
    // NB: libweb_rust runs these steps, see Rust/src/dom/character_data.rs.
    GC::Ptr<Text> new_node = static_cast<Text*>(RustFFI::rust_dom_split_text(this, offset));
    if (!new_node)
        return WebIDL::IndexSizeError::create("Replacement offset out of range."_utf16);
    return GC::Ref { *new_node };
}

// https://dom.spec.whatwg.org/#dom-text-wholetext
Utf16String Text::whole_text()
{
    // NB: libweb_rust runs these steps, see Rust/src/dom/character_data.rs.
    return Utf16String::adopt_raw(RustFFI::rust_dom_whole_text(this));
}

void Text::set_style_node_id(CSS::StyleNodeID style_node_id)
{
    if (m_core.style_node_id == style_node_id)
        return;
    auto old_style_node_id = m_core.style_node_id;
    m_core.style_node_id = style_node_id;
    Layout::Node::dom_node_style_node_changed(*this, old_style_node_id);
}

// https://html.spec.whatwg.org/multipage/dom.html#text-node-directionality
Optional<Element::Directionality> Text::directionality() const
{
    // 1. If text's data does not contain a code point whose bidirectional character type is L, AL, or R, then return null.
    // 2. Let codePoint be the first code point in text's data whose bidirectional character type is L, AL, or R.
    Optional<Unicode::BidiClass> found_character_bidi_class;
    for (auto code_point : data()) {
        auto bidi_class = Unicode::bidirectional_class(code_point);
        if (first_is_one_of(bidi_class, Unicode::BidiClass::LeftToRight, Unicode::BidiClass::RightToLeftArabic, Unicode::BidiClass::RightToLeft)) {
            found_character_bidi_class = bidi_class;
            break;
        }
    }
    if (!found_character_bidi_class.has_value())
        return {};

    // 3. If codePoint is of bidirectional character type AL or R, then return 'rtl'.
    if (first_is_one_of(*found_character_bidi_class, Unicode::BidiClass::RightToLeftArabic, Unicode::BidiClass::RightToLeft))
        return Element::Directionality::Rtl;

    // 4. If codePoint is of bidirectional character type L, then return 'ltr'.
    // NOTE: codePoint should always be of bidirectional character type L by this point, so we can just return 'ltr' here.
    VERIFY(*found_character_bidi_class == Unicode::BidiClass::LeftToRight);
    return Element::Directionality::Ltr;
}

}
