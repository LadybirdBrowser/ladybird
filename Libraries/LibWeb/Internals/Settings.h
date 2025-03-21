/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Internals/InternalsBase.h>

namespace Web::Internals {

class Settings final : public InternalsBase {
    WEB_PLATFORM_OBJECT(Settings, InternalsBase);
    GC_DECLARE_ALLOCATOR(Settings);

public:
    virtual ~Settings() override;

    void load_current_settings();
    void restore_default_settings();

    void set_new_tab_page_url(String const& new_tab_page_url);

    void load_available_search_engines();
    void set_search_engine(Optional<String> const& search_engine);

private:
    explicit Settings(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
