/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Variant.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>

namespace Web::CSS {

class LengthPercentage;
class LengthPercentageOrAuto;
class Size;

}

namespace Web::Layout {

class LayoutRustBridge {
public:
    LayoutRustBridge();
    ~LayoutRustBridge();

    void run_root_layout(Box& viewport, NodeWithStyleAndBoxModelMetrics* document_element_layout_node, CSSPixels viewport_inline_size, CSSPixels viewport_block_size, bool should_collect_devtools_layout_data);
    void compute_subtree_layout(Box&, Painting::Paintable& paintable_to_replace);
    void replay_saved_abspos_layout(Box&, Painting::Paintable& paintable_to_replace);

private:
    [[nodiscard]] RustFFI::FfiLayoutFcCallbacks formatting_context_callbacks();
    [[nodiscard]] RustFFI::FfiCommitSink commit_sink();

    struct LineCommitContext;
    Box const* m_commit_root { nullptr };
    OwnPtr<LineCommitContext> m_line_commit_context;
    RefPtr<Painting::Paintable> m_replaced_paintable;
    RefPtr<Painting::Paintable> m_commit_parent_paintable;
    RefPtr<Painting::Paintable> m_commit_insert_before_paintable;
};

[[nodiscard]] RustFFI::FfiSizeValue build_style_size_value(CSS::Size const&);
[[nodiscard]] RustFFI::FfiSizeValue build_style_size_value(CSS::LengthPercentage const&);
[[nodiscard]] RustFFI::FfiSizeValue build_style_size_value(CSS::LengthPercentageOrAuto const&);

[[nodiscard]] Optional<RustFFI::FfiFormattingContextType> formatting_context_type_created_by_box(Box const&);
[[nodiscard]] StringView formatting_context_type_name(RustFFI::FfiFormattingContextType);
[[nodiscard]] bool box_inset_properties_contain_anchor_functions(Box const&);
[[nodiscard]] bool can_replay_saved_abspos_layout_inputs_after_style_change(Box const&);

// True while a synchronous Rust layout pass (including its commit) is on the
// stack. Computed values must never be replaced in that window: the pass
// caches decoded style and borrows payload pointers that a replacement would
// invalidate under it.
[[nodiscard]] WEB_API bool layout_pass_currently_running();

}

// Non-null calc handles returned in FfiSizeValue retain their shared Rust
// StyleValueData allocation. This is their matching release hook.
extern "C" WEB_API void ladybird_layout_release_calc_handle(void const*);
// Releases one name-table reference transferred by FfiGridStyleFacts.
extern "C" WEB_API void ladybird_layout_release_grid_name_handle(size_t);
// Releases one position-anchor name reference transferred by a lazy style
// field decode.
extern "C" WEB_API void ladybird_layout_release_anchor_name_handle(size_t);

// Per-code-point classification lookups for the Rust text chunker. The
// line-break-class groupings implement the css-text-4 word-break policies.
extern "C" WEB_API u8 ladybird_layout_text_type_for_code_point(u32);
extern "C" WEB_API bool ladybird_layout_code_point_has_break_all_line_break_class(u32);
extern "C" WEB_API bool ladybird_layout_code_point_has_keep_all_line_break_class(u32);
extern "C" WEB_API bool ladybird_layout_code_point_has_combining_mark_line_break_class(u32);
extern "C" WEB_API bool ladybird_layout_code_point_has_emoji_property(u32);
