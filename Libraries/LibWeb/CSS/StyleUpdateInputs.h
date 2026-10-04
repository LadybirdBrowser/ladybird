/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// Proof that the layout of a document's container is up to date, which the document's viewport, media queries and
// viewport units read. Only lay_out_container_for_style_update() makes one.
class ContainerLayoutUpToDate {
    friend ContainerLayoutUpToDate lay_out_container_for_style_update(DOM::Document&);
    ContainerLayoutUpToDate() = default;
};

// Lays out the document's container where it, or a document above it, has style or layout work pending. A style update
// of the document does this first, ahead of its own style update scopes, as it is a read of the embedding document's.
ContainerLayoutUpToDate lay_out_container_for_style_update(DOM::Document&);

// Proof that a style update brought up to date and recorded what a document's style transaction is taken against: the
// layout of its container, its dirty style attributes, its viewport, its media rules, its user-agent and user sheets,
// the sample of an animation-only update, and its counter styles. Only begin_style_update_inputs() makes one, and the
// document takes or lets fly a transaction only with one, so a transaction taken without them does not compile.
class StyleUpdateInputs {
    AK_MAKE_NONCOPYABLE(StyleUpdateInputs);
    AK_MAKE_DEFAULT_MOVABLE(StyleUpdateInputs);

private:
    friend Optional<StyleUpdateInputs> begin_style_update_inputs(Layout::BegunRead const&, DOM::Document&, ContainerLayoutUpToDate);
    StyleUpdateInputs() = default;
};

// Brings up to date and records what a style transaction of the document is taken against, as a style update does
// before it takes one, and the seal of a transaction that flies before it lets it fly. Runs inside a style update of the
// document (StyleComputer::begin_style_update()). Answers nothing where the document's style is up to date once its
// animations are sampled, so that there is no transaction to take.
Optional<StyleUpdateInputs> begin_style_update_inputs(Layout::BegunRead const&, DOM::Document&, ContainerLayoutUpToDate);

}
