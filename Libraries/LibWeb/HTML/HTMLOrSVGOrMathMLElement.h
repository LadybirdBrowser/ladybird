/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/HTMLElement.h>
#include <LibWeb/HTML/DOMStringMap.h>

namespace Web::HTML {

template<typename ElementBase>
class HTMLOrSVGOrMathMLElement {
public:
    [[nodiscard]] GC::Ref<DOMStringMap> dataset();

    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#dom-noncedelement-nonce
    Utf16String const& nonce() const;
    void set_nonce(Utf16View nonce);

    void focus(Bindings::FocusOptions const& = {});
    void blur();

protected:
    void attribute_changed(Utf16FlyString const&, Optional<Utf16String> const&, Optional<Utf16String> const&, Optional<Utf16FlyString> const&);
    WebIDL::ExceptionOr<void> cloned(DOM::Node&, bool) const;
    void inserted();

private:
    GC::Ptr<DOMStringMap>& dataset_storage();
    Utf16String* cryptographic_nonce_storage();
    Utf16String const* cryptographic_nonce_storage() const;
    Utf16String& ensure_cryptographic_nonce_storage();
};

}
