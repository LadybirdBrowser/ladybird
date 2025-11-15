/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG2/types.html#InterfaceSVGNumber
class SVGNumber final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGNumber, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGNumber);

public:
    enum class ReadOnly : u8 {
        Yes,
        No,
    };

    [[nodiscard]] static GC::Ref<SVGNumber> create(JS::Realm&, float value, ReadOnly);
    virtual ~SVGNumber() override = default;

    float value() const { return m_value; }
    WebIDL::ExceptionOr<void> set_value(float value);

    ReadOnly read_only() const { return m_read_only; }

private:
    SVGNumber(JS::Realm&, float value, ReadOnly);

    virtual void initialize(JS::Realm&) override;

    float m_value { 0 };

    // https://www.w3.org/TR/SVG2/types.html#ReadOnlyNumber
    ReadOnly m_read_only;
};

}
