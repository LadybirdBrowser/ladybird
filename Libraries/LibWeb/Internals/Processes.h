/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Internals/InternalsBase.h>

namespace Web::Internals {

class Processes final : public InternalsBase {
    WEB_PLATFORM_OBJECT(Processes, InternalsBase);
    GC_DECLARE_ALLOCATOR(Processes);

public:
    virtual ~Processes() override;

    void update_process_statistics();

private:
    explicit Processes(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
