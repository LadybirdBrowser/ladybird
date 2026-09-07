/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/CSS/Descriptor.h>
#include <LibWeb/Export.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS {

class WEB_API RustDescriptorBlock {
    AK_MAKE_NONCOPYABLE(RustDescriptorBlock);

public:
    explicit RustDescriptorBlock(Vector<Descriptor>);
    explicit RustDescriptorBlock(Parser::ValueParserFFI::FfiDescriptorBlock*);
    ~RustDescriptorBlock();
    RustDescriptorBlock(RustDescriptorBlock&&);
    RustDescriptorBlock& operator=(RustDescriptorBlock&&);
    RustDescriptorBlock share() const;
    RustDescriptorBlock retain() const;
    void replace(RustDescriptorBlock const&);
    size_t size() const;
    Vector<Descriptor> const& descriptors() const;
    RefPtr<StyleValue const> descriptor(DescriptorNameAndID const&) const;
    RefPtr<StyleValue const> descriptor_or_initial_value(AtRuleID, DescriptorNameAndID const&) const;
    bool set(DescriptorNameAndID const&, StyleValue const&);
    bool remove(DescriptorNameAndID const&);
    size_t external_memory_size() const;
    auto* handle() const { return m_block; }

private:
    Parser::ValueParserFFI::FfiDescriptorBlock* m_block;
    mutable Vector<Descriptor> m_descriptors;
    mutable Optional<u64> m_view_revision;
};

}
