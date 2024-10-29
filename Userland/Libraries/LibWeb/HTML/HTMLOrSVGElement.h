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

    void focus();
    void blur();

protected:
    void visit_edges(JS::Cell::Visitor&);

    // https://html.spec.whatwg.org/multipage/dom.html#dom-dataset-dev
    JS::GCPtr<DOMStringMap> m_dataset;

    // https://html.spec.whatwg.org/multipage/interaction.html#locked-for-focus
    bool m_locked_for_focus { false };
};

}
