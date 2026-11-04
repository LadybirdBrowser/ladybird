/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/MediaList.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/CSS/RustMediaList.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom-1/#the-medialist-interface
class MediaList final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(MediaList, media_list)

public:
    // The new list shares the media queries of the list it is created with.
    [[nodiscard]] static GC::Ref<MediaList> create(RustMediaList const&);

    RustMediaList share_native_list() const;

    void set_associated_style_sheet(NonnullRefPtr<StyleSheetState>);
    void set_associated_rule(GC::Ref<CSSRule>);
};

void invalidate_style_sheet_for_media_change(StyleSheetState&);

}
