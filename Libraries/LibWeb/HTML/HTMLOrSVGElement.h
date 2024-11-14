/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/HTML/DOMStringMap.h>

namespace Web::HTML {

template<typename ElementBase>
class HTMLOrSVGElement {
public:
    [[nodiscard]] JS::NonnullGCPtr<DOMStringMap> dataset();

    // https://html.spec.whatwg.org/#dom-noncedelement-nonce
    String const& nonce() { return m_cryptographic_nonce; }
    void set_nonce(String const& nonce) { m_cryptographic_nonce = nonce; }

    void focus();
    void blur();

protected:
    void attribute_changed(FlyString const&, Optional<String> const&, Optional<String> const&, Optional<FlyString> const&);
    WebIDL::ExceptionOr<void> cloned(DOM::Node&, bool);
    void inserted();
    void visit_edges(JS::Cell::Visitor&);

    // https://html.spec.whatwg.org/multipage/dom.html#dom-dataset-dev
    JS::GCPtr<DOMStringMap> m_dataset;

    // https://html.spec.whatwg.org/#cryptographicnonce
    String m_cryptographic_nonce;

    // https://html.spec.whatwg.org/multipage/interaction.html#locked-for-focus
    bool m_locked_for_focus { false };
};

}
