/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/MediaQueryList.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/CSS/RustMediaList.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// 4.2. The MediaQueryList Interface, https://drafts.csswg.org/cssom-view/#the-mediaquerylist-interface
class MediaQueryList final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(MediaQueryList, media_query_list)

public:
    [[nodiscard]] static GC::Ref<MediaQueryList> create(DOM::Document&, RustMediaList const&);

    bool has_change_listener() const;
    void evaluate_and_report_change();
};

}
