/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API StyleSheetList final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(StyleSheetList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(StyleSheetList);

public:
    [[nodiscard]] static GC::Ref<StyleSheetList> create(StyleScope&);

    CSSStyleSheet* item(size_t index) const;
    size_t length() const;

private:
    explicit StyleSheetList(StyleScope&);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    StyleScope& m_scope;
};

}
