/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/StyleSheetState.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom-1/#the-stylesheet-interface
class WEB_API StyleSheet : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(StyleSheet, Bindings::GCAllocatedWrappable);

public:
    virtual ~StyleSheet() override = default;

    StyleSheetState& state() { return m_state; }
    StyleSheetState const& state() const { return m_state; }

    Utf16FlyString type() const { return m_state->type(); }
    DOM::Element* owner_node() { return m_state->owner_node(); }
    Optional<Utf16String> href_for_bindings() const { return m_state->href_for_bindings(); }
    Optional<Utf16String> title_for_bindings() const { return m_state->title_for_bindings(); }
    GC::Ref<MediaList> media() const { return m_state->media(); }
    bool disabled() const { return m_state->disabled(); }
    void set_disabled(bool disabled) { m_state->set_disabled(disabled); }
    CSSStyleSheet* parent_style_sheet() const;

protected:
    explicit StyleSheet(StyleSheetState&);
    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual size_t external_memory_size() const override;

private:
    NonnullRefPtr<StyleSheetState> m_state;
};

}
