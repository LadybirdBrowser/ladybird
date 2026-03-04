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
    WEB_PLATFORM_OBJECT(CSSFunctionDescriptors, CSSDescriptors);
    GC_DECLARE_ALLOCATOR(CSSFunctionDescriptors);

public:
    [[nodiscard]] static GC::Ref<CSSFunctionDescriptors> create(JS::Realm&, Vector<Descriptor>);

    virtual ~CSSFunctionDescriptors() override = default;

    virtual void initialize(JS::Realm&) override;

    String result() const;
    WebIDL::ExceptionOr<void> set_result(StringView value);

private:
    CSSFunctionDescriptors(JS::Realm& realm, Vector<Descriptor> descriptors)
        : CSSDescriptors(realm, AtRuleID::Function, move(descriptors))
    {
    }
};

}
