/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Text.h>

namespace Web::DOM {

// NB: libweb_rust defines, allocates and destroys every CDATASection, see Rust/src/dom/node_objects.rs.
class CDATASection final : public Text {
    WEB_WRAPPABLE(CDATASection, Text);

public:
    [[nodiscard]] static GC::Ref<CDATASection> create(Document&, Utf16String data);

    // ^Node
    virtual Utf16FlyString node_name() const override { return "#cdata-section"_utf16_fly_string; }

private:
    friend struct RustNodeObjectHooks;

    explicit CDATASection(VtableProbe);
    virtual ~CDATASection() override = default;
};

}
