/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/HTML/DOMStringMap.h>

namespace Web::HTML {

template<typename ElementBase>
class HTMLOrSVGElement {
public:
    [[nodiscard]] GC::Ref<DOMStringMap> dataset();

    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#dom-noncedelement-nonce
    String const& nonce() { return m_cryptographic_nonce; }
    void set_nonce(String const& nonce) { m_cryptographic_nonce = nonce; }

    void focus();
    void blur();

protected:
    void attribute_changed(FlyString const&, Optional<String> const&, Optional<String> const&, Optional<FlyString> const&);
    WebIDL::ExceptionOr<void> cloned(DOM::Node&, bool) const;
    void inserted();
    void visit_edges(JS::Cell::Visitor&);

    // https://html.spec.whatwg.org/multipage/dom.html#dom-dataset-dev
    GC::Ptr<DOMStringMap> m_dataset;

    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#cryptographicnonce
    String m_cryptographic_nonce;

    // https://html.spec.whatwg.org/multipage/interaction.html#locked-for-focus
    bool m_locked_for_focus { false };
};

}
