/*
 * Copyright (c) 2025, Bohdan Sverdlov <freezar92@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class AnonymousImageBox final : public ReplacedBox {
    GC_CELL(AnonymousImageBox, ReplacedBox);
    GC_DECLARE_ALLOCATOR(AnonymousImageBox);

public:
    AnonymousImageBox(DOM::Document&, DOM::Element&, GC::Ref<CSS::ComputedProperties>);
    virtual ~AnonymousImageBox() override;

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual void visit_edges(Visitor&) override;
};

}
