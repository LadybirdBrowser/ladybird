/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Crypto/SubtleCrypto.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Crypto {

class Crypto : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Crypto, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Crypto);

public:
    [[nodiscard]] static JS::NonnullGCPtr<Crypto> create(JS::Realm&);

    virtual ~Crypto() override;

    JS::NonnullGCPtr<SubtleCrypto> subtle() const;

    WebIDL::ExceptionOr<JS::Handle<WebIDL::ArrayBufferView>> get_random_values(JS::Handle<WebIDL::ArrayBufferView>) const;
    WebIDL::ExceptionOr<String> random_uuid() const;

protected:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    explicit Crypto(JS::Realm&);

    JS::GCPtr<SubtleCrypto> m_subtle;
};

ErrorOr<String> generate_random_uuid();

}
