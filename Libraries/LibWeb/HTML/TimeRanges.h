/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

class TimeRanges final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TimeRanges, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(TimeRanges);

public:
    size_t length() const;
    double start(u32 index) const;
    double end(u32 index) const;

private:
    explicit TimeRanges(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
