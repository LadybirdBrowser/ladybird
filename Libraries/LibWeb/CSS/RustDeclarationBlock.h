/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS {

// The declarations live in Rust. These document-thread views retain facade-local resource state.
class WEB_API RustDeclarationBlock {
    AK_MAKE_NONCOPYABLE(RustDeclarationBlock);

public:
    RustDeclarationBlock(Vector<StyleProperty>, OrderedHashMap<Utf16FlyString, StyleProperty>);
    explicit RustDeclarationBlock(Parser::ValueParserFFI::FfiDeclarationBlock*);
    ~RustDeclarationBlock();

    RustDeclarationBlock(RustDeclarationBlock&&);
    RustDeclarationBlock& operator=(RustDeclarationBlock&&);
    // Fork an independent copy-on-write owner of the current immutable data.
    RustDeclarationBlock share() const;
    // Observe the same document-thread owner, including subsequent mutations.
    RustDeclarationBlock retain() const;
    void replace(RustDeclarationBlock const&);
    bool is_empty() const;
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

    Parser::ValueParserFFI::FfiDeclarationBlock const* handle() const { return m_block; }

private:
    void update_views() const;

    Parser::ValueParserFFI::FfiDeclarationBlock* m_block;
    mutable Vector<StyleProperty> m_properties;
    mutable OrderedHashMap<Utf16FlyString, StyleProperty> m_custom_properties;
    mutable Optional<u64> m_view_revision;
};

}
