/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSDescriptors.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-mixins-1/#cssfunctiondescriptors
class CSSFunctionDescriptors final : public CSSDescriptors {
    WEB_WRAPPABLE(CSSFunctionDescriptors, CSSDescriptors);
    GC_DECLARE_ALLOCATOR(CSSFunctionDescriptors);

public:
    [[nodiscard]] static GC::Ref<CSSFunctionDescriptors> create(Vector<Descriptor>);

    virtual ~CSSFunctionDescriptors() override = default;

    String result() const;
    WebIDL::ExceptionOr<void> set_result(JS::Realm&, StringView value);

private:
    explicit CSSFunctionDescriptors(Vector<Descriptor> descriptors)
        : CSSDescriptors(AtRuleID::Function, move(descriptors))
    {
    }
};

}
