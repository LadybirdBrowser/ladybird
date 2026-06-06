/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibWeb/DOM/Text.h>

namespace Web::DOM {

class CDATASection final : public Text {
    WEB_WRAPPABLE(CDATASection, Text);
    GC_DECLARE_ALLOCATOR(CDATASection);

public:
    [[nodiscard]] static GC::Ref<CDATASection> create(Document&, Utf16String data);
    virtual ~CDATASection() override;

    // ^Node
    virtual FlyString node_name() const override { return "#cdata-section"_fly_string; }

private:
    CDATASection(Document&, Utf16String);
};

template<>
inline bool Node::fast_is<CDATASection>() const { return is_cdata_section(); }

}
