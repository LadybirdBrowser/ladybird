/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::Fetch {

// https://fetch.spec.whatwg.org/#fetchlaterresult
class FetchLaterResult final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(FetchLaterResult, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(FetchLaterResult);

public:
    [[nodiscard]] static GC::Ref<FetchLaterResult> create(JS::Realm&);
    virtual ~FetchLaterResult() override;

    [[nodiscard]] bool activated() const { return m_activated; }
    void set_activated(bool activated) { m_activated = activated; }

private:
    explicit FetchLaterResult(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    // https://fetch.spec.whatwg.org/#dom-fetchlaterresult-activated
    bool m_activated { false };
};

}
