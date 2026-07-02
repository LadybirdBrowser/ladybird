/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/SVGImageBox.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Layout/SVGTextBox.h>
#include <LibWeb/Layout/SVGTextPathBox.h>

namespace Web::Layout {

class SVGFormattingContext final : public FormattingContext {
public:
    explicit SVGFormattingContext(LayoutState&, LayoutMode, Box const&, FormattingContext* parent, Gfx::AffineTransform parent_viewbox_transform = {}, Optional<Gfx::AffineTransform> parent_svg_transform = {});
    ~SVGFormattingContext();

    virtual void run(LayoutInput const&) override;
    virtual CSSPixels automatic_content_width() const override;
    virtual CSSPixels automatic_content_height() const override;

private:
    void layout_svg_element(Box const&, LayoutInput const&, Gfx::AffineTransform const& parent_svg_transform);
    void layout_nested_viewport(Box const&, Gfx::AffineTransform const& parent_svg_transform);
    void layout_container_element(SVGBox const&, LayoutInput const&, Gfx::AffineTransform const& container_svg_transform);
    void layout_graphics_element(SVGGraphicsBox const&, LayoutInput const&, Gfx::AffineTransform const& parent_svg_transform);
    void layout_path_like_element(SVGGraphicsBox const&, LayoutInput const&);
    void layout_mask_or_clip(SVGBox const&);
    void layout_image_element(SVGImageBox const& image_box);

    [[nodiscard]] Gfx::Path compute_path_for_text(SVGTextBox const&) const;
    [[nodiscard]] Gfx::Path compute_path_for_text_path(SVGTextPathBox const&) const;

    Gfx::AffineTransform m_parent_viewbox_transform {};
    Optional<Gfx::AffineTransform> m_parent_svg_transform {};

    Optional<AvailableSpace> m_available_space {};
    Optional<CSSPixels> m_quirks_mode_percentage_basis_height {};
    Gfx::AffineTransform m_current_viewbox_transform {};
    CSSPixelSize m_viewport_size {};
    CSSPixelPoint m_svg_offset {};
    Gfx::FloatPoint m_current_text_position {};
};

}
