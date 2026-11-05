/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/FontFaceSet.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-font-loading/#fontfaceset
class WEB_API FontFaceSet final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(FontFaceSet, font_face_set)

public:
    [[nodiscard]] static GC::Ref<FontFaceSet> create(HTML::EnvironmentSettingsObject&);

    size_t set_size();

    void add_css_connected_font(NonnullRefPtr<FontFaceState>);
    void remove_css_connected_font(NonnullRefPtr<FontFaceState>);
    void synchronize_css_connected_font_order();

    HTML::EnvironmentSettingsObject& relevant_settings_object();

    void set_is_pending_on_the_environment(bool);

    // What the set does when a font in it starts to load, and when that load ended.
    void font_started_loading(FontFaceState&);
    void font_finished_loading(FontFaceState&, bool succeeded);
};

}
