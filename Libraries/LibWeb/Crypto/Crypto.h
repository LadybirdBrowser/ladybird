/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Crypto/SubtleCrypto.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Crypto {

class Crypto : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Crypto, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Crypto);

public:
    [[nodiscard]] static GC::Ref<Crypto> create(JS::Realm&);

    virtual ~Crypto() override;

    GC::Ref<SubtleCrypto> subtle() const;

    WebIDL::ExceptionOr<WebIDL::ArrayBufferViewVariant> get_random_values(WebIDL::ArrayBufferViewVariant array) const;

    String random_uuid() const;

protected:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    explicit Crypto(JS::Realm&);

    GC::Ptr<SubtleCrypto> m_subtle;
};

WEB_API String generate_random_uuid();

}
