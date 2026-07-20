/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>

namespace Web::Editing {

// A clipboard fragment together with the structural information that is lost once its children are inserted.
// Keeping that information here lets replacement decisions remain independent from parsing and serialization quirks.
class ReplacementFragment {
    AK_MAKE_NONCOPYABLE(ReplacementFragment);

public:
    ReplacementFragment(DOM::Range&, TrustedTypes::TrustedHTMLOrString const&);
    ReplacementFragment(ReplacementFragment&&) = default;

    bool contains_block_content() const { return m_contains_block_content; }
    bool contains_only_text() const { return m_contains_only_text; }
    bool has_interchange_newline_at_start() const { return m_has_interchange_newline_at_start; }
    bool has_interchange_newline_at_end() const { return m_has_interchange_newline_at_end; }

    DOM::DocumentFragment& fragment() { return *m_fragment; }
    GC::RootVector<GC::Ref<DOM::Node>> children() const;

private:
    GC::Root<DOM::DocumentFragment> m_fragment;
    bool m_contains_block_content { false };
    bool m_contains_only_text { false };
    bool m_has_interchange_newline_at_start { false };
    bool m_has_interchange_newline_at_end { false };
};

}
