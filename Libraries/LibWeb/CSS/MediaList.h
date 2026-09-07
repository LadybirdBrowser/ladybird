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
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/RustMediaList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom-1/#the-medialist-interface
class MediaList final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(MediaList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(MediaList);

public:
    [[nodiscard]] static GC::Ref<MediaList> create(RustMediaList);
    virtual ~MediaList() override;

    Utf16String media_text() const;
    void set_media_text(Utf16View);
    size_t length() const { return m_media.length(); }
    Optional<Utf16String> item(u32 index) const;
    void append_medium(Utf16View);
    WebIDL::ExceptionOr<void> delete_medium(Utf16View);

    RustMediaList const& native_list() const { return m_media; }

    void set_associated_style_sheet(NonnullRefPtr<StyleSheetState>);

    // A media list belongs either to a sheet or to an `@media` rule inside one. Both are gates on
    // whether rules apply, so both have to say when the gate moves - a rule's list said nothing at
    // all, and changing a group's media therefore changed no style.
    void set_associated_rule(GC::Ref<CSSRule> rule) { m_associated_rule = rule; }

private:
    MediaList(RustMediaList);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    RefPtr<StyleSheetState> owning_style_sheet();
    void invalidate_owners_for_media_change();

    RefPtr<StyleSheetState> m_associated_style_sheet;
    GC::Ptr<CSSStyleSheet> m_associated_cssom_sheet;
    GC::Ptr<CSSRule> m_associated_rule;
    RustMediaList m_media;
};

void invalidate_style_sheet_for_media_change(StyleSheetState&);

}
