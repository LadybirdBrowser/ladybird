/*
 * Copyright (c) 2024, Colin Reeder <colin@vpzom.click>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::NavigationTiming {

class PerformanceNavigation final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(PerformanceNavigation, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(PerformanceNavigation);

public:
    ~PerformanceNavigation();

    u16 type() const;
    u16 redirect_count() const;

private:
    explicit PerformanceNavigation(JS::Realm&, u16 type, u16 redirect_count);

    void initialize(JS::Realm&) override;

    u16 m_type;
    u16 m_redirect_count;
};

}
