/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class HTMLTableColElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLTableColElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLTableColElement);

public:
    virtual ~HTMLTableColElement() override;

    WebIDL::UnsignedLong span() const;
    WebIDL::ExceptionOr<void> set_span(WebIDL::UnsignedLong);

private:
    HTMLTableColElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;
};

}
