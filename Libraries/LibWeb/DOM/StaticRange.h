/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/AbstractRange.h>

namespace Web::DOM {

class StaticRange final : public AbstractRange {
    WEB_PLATFORM_OBJECT(StaticRange, AbstractRange);
    GC_DECLARE_ALLOCATOR(StaticRange);

public:
    static WebIDL::ExceptionOr<GC::Ref<StaticRange>> construct_impl(JS::Realm&, Bindings::StaticRangeInit const&);

    StaticRange(Node& start_container, u32 start_offset, Node& end_container, u32 end_offset);
    virtual ~StaticRange() override;

    virtual void initialize(JS::Realm&) override;
};

}
