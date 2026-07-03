/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// A selectors string parsed for use by querySelector(All), matches() and closest().
// Documents cache these per selector string, so one SelectorQuery may be reused by many queries.
class SelectorQuery : public RefCounted<SelectorQuery> {
public:
    static NonnullRefPtr<SelectorQuery> create(CSS::SelectorList&& selectors)
    {
        return adopt_ref(*new SelectorQuery(move(selectors)));
    }

    CSS::SelectorList const& selectors() const { return m_selectors; }

    GC::Ptr<Element> query_first(ParentNode&) const;
    GC::Ref<NodeList> query_all(ParentNode&) const;

private:
    explicit SelectorQuery(CSS::SelectorList&&);

    CSS::SelectorList m_selectors;
};

}
