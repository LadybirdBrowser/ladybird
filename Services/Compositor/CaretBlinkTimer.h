/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibCompositing/Forward.h>
#include <LibCore/Forward.h>
#include <LibGfx/Rect.h>

namespace Compositor {

// Repaints the blinking caret of a display list each time its blink cycle turns it on or off.
class CaretBlinkTimer {
    AK_MAKE_NONCOPYABLE(CaretBlinkTimer);
    AK_MAKE_NONMOVABLE(CaretBlinkTimer);

public:
    CaretBlinkTimer(Function<Gfx::IntRect()> damage_rect_of_blinking_carets, Function<void(Gfx::IntRect)> schedule_repaint);
    ~CaretBlinkTimer();

    void did_install_display_list(Compositing::DisplayList const&);

private:
    void schedule_next_blink();

    Function<Gfx::IntRect()> m_damage_rect_of_blinking_carets;
    Function<void(Gfx::IntRect)> m_schedule_repaint;
    RefPtr<Core::Timer> m_timer;
    Optional<i64> m_blink_cycle_start_time_ns;
};

}
