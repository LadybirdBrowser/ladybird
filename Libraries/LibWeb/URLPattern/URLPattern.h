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

using URLPatternInput = Variant<String, Bindings::URLPatternInit>;

// https://urlpattern.spec.whatwg.org/#urlpattern
class URLPattern : public Bindings::Wrappable {
    WEB_WRAPPABLE(URLPattern, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(URLPattern);

public:
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> create(URLPatternInput const&, String const& base_url, Bindings::URLPatternOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<URLPattern>> create(URLPatternInput const&, Bindings::URLPatternOptions const&);

    WebIDL::ExceptionOr<bool> test(URLPatternInput const&, Optional<String> const& base_url) const;
    WebIDL::ExceptionOr<Optional<Bindings::URLPatternResult>> exec(URLPatternInput const&, Optional<String> const& base_url) const;

    String const& protocol() const;
    String const& username() const;
    String const& password() const;
    String const& hostname() const;
    String const& port() const;
    String const& pathname() const;
    String const& search() const;
    String const& hash() const;

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
