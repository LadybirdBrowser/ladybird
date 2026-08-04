/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/Bindings/URLPattern.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::URLPattern {

using URLPatternInput = Variant<Utf16String, Bindings::URLPatternInit>;

// https://urlpattern.spec.whatwg.org/#urlpattern
class URLPattern : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(URLPattern, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(URLPattern);

public:
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> create(URLPatternInput const&, Utf16String const& base_url, Bindings::URLPatternOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> create(URLPatternInput const&, Bindings::URLPatternOptions const&);

    WebIDL::ExceptionOr<bool> test(URLPatternInput const&, Optional<Utf16String> const& base_url) const;
    WebIDL::ExceptionOr<Optional<Bindings::URLPatternResult>> exec(URLPatternInput const&, Optional<Utf16String> const& base_url) const;

    Utf16String protocol() const;
    Utf16String username() const;
    Utf16String password() const;
    Utf16String hostname() const;
    Utf16String port() const;
    Utf16String pathname() const;
    Utf16String search() const;
    Utf16String hash() const;

    bool has_reg_exp_groups() const;

    virtual ~URLPattern() override;

protected:
    explicit URLPattern(URL::RustIntegration::URLPattern);

private:
    // https://urlpattern.spec.whatwg.org/#ref-for-url-pattern%E2%91%A0
    // Each URLPattern has an associated URL pattern, a URL pattern.
    URL::RustIntegration::URLPattern m_url_pattern;
};

}
