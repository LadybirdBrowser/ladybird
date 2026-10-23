/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS {

// A read-only Arc<DeclarationBlockData>. The cascade never needs a live CSSOM owner.
class WEB_API RustDeclarationBlockSnapshot {
    AK_MAKE_NONCOPYABLE(RustDeclarationBlockSnapshot);

public:
    explicit RustDeclarationBlockSnapshot(Parser::ValueParserFFI::DeclarationBlockData const*);
    ~RustDeclarationBlockSnapshot();
    RustDeclarationBlockSnapshot(RustDeclarationBlockSnapshot&&);
    RustDeclarationBlockSnapshot& operator=(RustDeclarationBlockSnapshot&&);

    Parser::ValueParserFFI::DeclarationBlockData const* data() const { return m_data; }

private:
    Parser::ValueParserFFI::DeclarationBlockData const* m_data;
};

// The declarations live in Rust. These document-thread views retain facade-local resource state.
class WEB_API RustDeclarationBlock {
    AK_MAKE_NONCOPYABLE(RustDeclarationBlock);

public:
    RustDeclarationBlock(Vector<StyleProperty>, OrderedHashMap<Utf16FlyString, StyleProperty>);
    explicit RustDeclarationBlock(Parser::ValueParserFFI::DeclarationBlock*);
    ~RustDeclarationBlock();

    RustDeclarationBlock(RustDeclarationBlock&&);
    RustDeclarationBlock& operator=(RustDeclarationBlock&&);
    // Fork an independent copy-on-write owner of the current immutable data.
    RustDeclarationBlock share() const;
    // Observe the same document-thread owner, including subsequent mutations.
    RustDeclarationBlock retain() const;
    void replace(RustDeclarationBlock const&);
    // True while both blocks hold the same immutable data, e.g. after replace() and before either changes.
    bool shares_declarations_with(RustDeclarationBlock const&) const;
    bool is_empty() const;
    // Whether a declaration holds an image or a gradient, as its value or as an item of its list.
    bool has_abstract_image_values() const;
    u64 identity() const;
    u64 revision() const;

    Vector<StyleProperty> const& properties() const;
    OrderedHashMap<Utf16FlyString, StyleProperty> const& custom_properties() const;
    size_t external_memory_size() const;

    bool set(PropertyID, StyleValue const&, Important);
    void append(PropertyID, StyleValue const&, Important);
    void set_custom(Utf16FlyString const&, StyleProperty const&);
    bool remove(PropertyID);
    bool remove_custom(Utf16FlyString const&);

    Parser::ValueParserFFI::DeclarationBlock const* handle() const { return m_block; }
    // Hands the block to the caller, who destroys it with rust_declaration_block_destroy(). The views are dropped.
    [[nodiscard]] Parser::ValueParserFFI::DeclarationBlock* leak_handle() &&;

private:
    void update_views() const;

    // NB: Most blocks are never viewed from C++, and an element's inline style is one per element, so the views are
    //     made on first use rather than carried empty by every block.
    struct Views {
        AK_ALLOC_WITH_KMALLOC;
        Vector<StyleProperty> properties;
        OrderedHashMap<Utf16FlyString, StyleProperty> custom_properties;
        Optional<u64> revision;
    };

    Parser::ValueParserFFI::DeclarationBlock* m_block;
    mutable OwnPtr<Views> m_views;
};

}
