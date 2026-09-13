/*
 * Copyright (c) 2026, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/CSS/StyleSheetState.h>

namespace Web::CSS {

struct SharedCompiledStyleSheetKey {
    FlatPtr contents_identity;
    String base_url;

    bool operator==(SharedCompiledStyleSheetKey const&) const = default;
};

class SharedCompiledStyleSheet final : public RefCounted<SharedCompiledStyleSheet> {
public:
    SharedCompiledStyleSheet(SharedCompiledStyleSheetKey key, NonnullRefPtr<StyleSheetState> contents, SheetID sheet_id)
        : m_key(move(key))
        , m_contents(move(contents))
        , m_sheet_id(sheet_id)
    {
    }

    SharedCompiledStyleSheetKey const& key() const { return m_key; }
    StyleSheetState& contents() { return m_contents; }
    SheetID sheet_id() const { return m_sheet_id; }

    bool is_attached_to(TreeScopeID tree_scope) const { return m_attachment_counts.contains(tree_scope); }
    void add_attachment(TreeScopeID tree_scope)
    {
        ++m_attachment_counts.ensure(tree_scope, [] { return 0; });
    }
    void remove_attachment(TreeScopeID tree_scope)
    {
        auto entry = m_attachment_counts.find(tree_scope);
        VERIFY(entry != m_attachment_counts.end());
        if (--entry->value == 0)
            m_attachment_counts.remove(entry);
    }
    bool has_attachments() const { return !m_attachment_counts.is_empty(); }

private:
    SharedCompiledStyleSheetKey m_key;
    NonnullRefPtr<StyleSheetState> m_contents;
    SheetID m_sheet_id;
    HashMap<TreeScopeID, size_t> m_attachment_counts;
};

}

template<>
struct AK::Traits<Web::CSS::SharedCompiledStyleSheetKey> : public DefaultTraits<Web::CSS::SharedCompiledStyleSheetKey> {
    static unsigned hash(Web::CSS::SharedCompiledStyleSheetKey const& key)
    {
        return pair_int_hash(Traits<FlatPtr>::hash(key.contents_identity), key.base_url.hash());
    }
};
