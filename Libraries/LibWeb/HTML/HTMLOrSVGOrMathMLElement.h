/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/HTML/DOMStringMap.h>

namespace Web::HTML {

template<typename ElementBase>
class HTMLOrSVGOrMathMLElement {
public:
    [[nodiscard]] GC::Ref<DOMStringMap> dataset();

    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#dom-noncedelement-nonce
    Utf16String const& nonce() const { return m_cryptographic_nonce; }
    void set_nonce(Utf16View nonce) { m_cryptographic_nonce = Utf16String::from_utf16(nonce); }

    void focus();
    void blur();

protected:
    void attribute_changed(Utf16FlyString const&, Optional<Utf16String> const&, Optional<Utf16String> const&, Optional<Utf16FlyString> const&);
    WebIDL::ExceptionOr<void> cloned(DOM::Node&, bool) const;
    void inserted();
    void visit_edges(JS::Cell::Visitor&);

    // https://html.spec.whatwg.org/multipage/dom.html#dom-dataset-dev
    GC::Ptr<DOMStringMap> m_dataset;

    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#cryptographicnonce
    Utf16String m_cryptographic_nonce;
};

}
