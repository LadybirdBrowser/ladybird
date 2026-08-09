/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom-1/#the-medialist-interface
class MediaList final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(MediaList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(MediaList);

public:
    [[nodiscard]] static GC::Ref<MediaList> create(Vector<NonnullRefPtr<MediaQuery>>&&);
    virtual ~MediaList() override = default;

    Utf16String media_text() const;
    void set_media_text(Utf16View);
    size_t length() const { return m_media.size(); }
    Optional<Utf16String> item(u32 index) const;
    void append_medium(Utf16View);
    WebIDL::ExceptionOr<void> delete_medium(Utf16View);

    bool evaluate(DOM::Document const&);
    bool matches() const;

    void set_associated_style_sheet(GC::Ref<StyleSheet> style_sheet) { m_associated_style_sheet = style_sheet; }

    // A media list belongs either to a sheet or to an `@media` rule inside one. Both are gates on
    // whether rules apply, so both have to say when the gate moves - a rule's list said nothing at
    // all, and changing a group's media therefore changed no style.
    void set_associated_rule(GC::Ref<CSSRule> rule) { m_associated_rule = rule; }

    void dump(StringBuilder&, int indent_levels = 0) const;

private:
    MediaList(Vector<NonnullRefPtr<MediaQuery>>&&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<CSSStyleSheet> owning_style_sheet();
    void invalidate_owners_for_media_change();

    GC::Ptr<StyleSheet> m_associated_style_sheet;
    GC::Ptr<CSSRule> m_associated_rule;
    Vector<NonnullRefPtr<MediaQuery>> m_media;
};

}
