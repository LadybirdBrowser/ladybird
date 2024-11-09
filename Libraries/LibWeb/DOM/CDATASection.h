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
    WEB_PLATFORM_OBJECT(CDATASection, Text);
    JS_DECLARE_ALLOCATOR(CDATASection);

public:
    virtual ~CDATASection() override;

    // ^Node
    virtual FlyString node_name() const override { return "#cdata-section"_fly_string; }

private:
    CDATASection(Document&, String const&);

    virtual void initialize(JS::Realm&) override;
};

template<>
inline bool Node::fast_is<CDATASection>() const { return is_cdata_section(); }

}
