/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Crypto/SubtleCrypto.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Crypto {

class Crypto : public Bindings::Wrappable {
    WEB_WRAPPABLE(Crypto, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Crypto);

public:
    [[nodiscard]] static GC::Ref<Crypto> create();

    virtual ~Crypto() override;

    GC::Ref<SubtleCrypto> subtle() const;

    WebIDL::ExceptionOr<WebIDL::ArrayBufferViewVariant> get_random_values(WebIDL::ArrayBufferViewVariant) const;
    String random_uuid() const;

protected:
    virtual void visit_edges(GC::Cell::Visitor&) override;

private:
    Crypto();

    GC::Ptr<SubtleCrypto> m_subtle;
};

WEB_API String generate_random_uuid();

}
