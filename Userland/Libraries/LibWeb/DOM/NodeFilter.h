/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::DOM {

class NodeFilter final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(NodeFilter, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(NodeFilter);

public:
    [[nodiscard]] static JS::NonnullGCPtr<NodeFilter> create(JS::Realm&, WebIDL::CallbackType&);

    virtual ~NodeFilter() = default;

    WebIDL::CallbackType& callback() { return m_callback; }

    // FIXME: Generate both of these enums from IDL.
    enum class Result : u8 {
        FILTER_ACCEPT = 1,
        FILTER_REJECT = 2,
        FILTER_SKIP = 3,
    };

    enum class WhatToShow : u32 {
        SHOW_ALL = 0xFFFFFFFF,
        SHOW_ELEMENT = 0x1,
        SHOW_ATTRIBUTE = 0x2,
        SHOW_TEXT = 0x4,
        SHOW_CDATA_SECTION = 0x8,
        SHOW_PROCESSING_INSTRUCTION = 0x40,
        SHOW_COMMENT = 0x80,
        SHOW_DOCUMENT = 0x100,
        SHOW_DOCUMENT_TYPE = 0x200,
        SHOW_DOCUMENT_FRAGMENT = 0x400,
    };

private:
    NodeFilter(JS::Realm&, WebIDL::CallbackType&);

    virtual void visit_edges(Cell::Visitor&) override;

    JS::NonnullGCPtr<WebIDL::CallbackType> m_callback;
};

AK_ENUM_BITWISE_OPERATORS(NodeFilter::WhatToShow);

}
