/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>

namespace Web::Layout {

class Viewport final : public Box {
public:
    explicit Viewport(DOM::Document&, CSS::LayoutStyle);
    virtual ~Viewport() override;

    DOM::Document const& dom_node() const;

private:
    virtual bool is_viewport() const override { return true; }
};

template<>
inline bool Node::fast_is<Viewport>() const { return is_viewport(); }

}
