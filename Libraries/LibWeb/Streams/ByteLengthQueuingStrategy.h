/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/ByteLengthQueuingStrategy.h>
#include <LibWeb/Bindings/QueuingStrategyInit.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#bytelengthqueuingstrategy
class ByteLengthQueuingStrategy final : public Bindings::Wrappable {
    WEB_WRAPPABLE(ByteLengthQueuingStrategy, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(ByteLengthQueuingStrategy);

public:
    static GC::Ref<ByteLengthQueuingStrategy> construct_impl(Bindings::QueuingStrategyInit const&);

    virtual ~ByteLengthQueuingStrategy() override;

    // https://streams.spec.whatwg.org/#blqs-high-water-mark
    double high_water_mark() const
    {
        // The highWaterMark getter steps are:
        // 1. Return this.[[highWaterMark]].
        return m_high_water_mark;
    }

    GC::Ref<WebIDL::CallbackType> size(JS::Realm&);

private:
    explicit ByteLengthQueuingStrategy(double high_water_mark);

    // https://streams.spec.whatwg.org/#bytelengthqueuingstrategy-highwatermark
    double m_high_water_mark { 0 };
};

}
