/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibWeb/HTML/SelectItem.h>

#include <QMenu>
#include <QPoint>

namespace Ladybird {

// The popup menu for a <select> element. Every open() ends in exactly one on_closed() call — carrying the chosen item's
// id, or nothing when the menu went away without a choice — unless close_without_reporting() took the menu down.
class SelectDropdown final : public QMenu {
public:
    explicit SelectDropdown(QWidget* parent);

    void open(QPoint const& global_position, int minimum_width, Vector<Web::HTML::SelectItem> const&);
    void close_without_reporting();

    Function<void(Optional<u32> const& selected_item_id)> on_closed;

private:
    bool m_suppress_close_report { false };
};

}
