/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
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

namespace Web::Painting {

struct UsedGridTrackList;

}

namespace Web::Layout {

class LayoutRustBridge {
public:
    LayoutRustBridge();
    ~LayoutRustBridge();

    void run_root_layout(Box& viewport, CSSPixels viewport_inline_size, CSSPixels viewport_block_size, bool should_collect_devtools_layout_data);
    void compute_subtree_layout(Box&);
    void replay_saved_abspos_layout(Box&);

private:
    [[nodiscard]] RustFFI::FfiLayoutFcCallbacks formatting_context_callbacks();
    [[nodiscard]] RustFFI::FfiCommitSink commit_sink();

    Box const* m_commit_root { nullptr };
};

[[nodiscard]] Optional<RustFFI::FfiFormattingContextType> formatting_context_type_created_by_box(Box const&);
[[nodiscard]] StringView formatting_context_type_name(RustFFI::FfiFormattingContextType);
[[nodiscard]] bool can_replay_saved_abspos_layout_inputs_after_style_change(Box const&);

[[nodiscard]] Painting::UsedGridTrackList build_used_grid_track_list(RustFFI::FfiUsedGridTrackList const&);

// True while a synchronous Rust layout pass (including its commit) is on the
// stack. Computed values must never be replaced in that window: the pass
// caches decoded style and borrows payload pointers that a replacement would
// invalidate under it.
[[nodiscard]] WEB_API bool layout_pass_currently_running();

}

// Per-code-point classification lookups for the Rust text chunker. The
// line-break-class groupings implement the css-text-4 word-break policies.
extern "C" WEB_API u8 ladybird_layout_text_type_for_code_point(u32);
extern "C" WEB_API bool ladybird_layout_code_point_has_break_all_line_break_class(u32);
extern "C" WEB_API bool ladybird_layout_code_point_has_keep_all_line_break_class(u32);
extern "C" WEB_API bool ladybird_layout_code_point_has_combining_mark_line_break_class(u32);
extern "C" WEB_API bool ladybird_layout_code_point_has_emoji_property(u32);
extern "C" WEB_API size_t ladybird_layout_text_node_dom_offset_for_rendered_text_offset(void*, size_t, bool use_end_boundary);
extern "C" WEB_API size_t ladybird_layout_text_node_rendered_text_offset_for_dom_offset(void*, size_t, bool use_end_boundary);

extern "C" WEB_API void ladybird_layout_node_shell_release(void*);
