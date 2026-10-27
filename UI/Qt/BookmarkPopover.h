/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <UI/Qt/Popover.h>

class QComboBox;
class QLabel;
class QLineEdit;

namespace Ladybird {

// The bookmark of the current page, opened from the star in the address field: its name, its folder, and a way to
// remove it. Changes are kept when the popover closes, unless the bookmark was removed.
class BookmarkPopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit BookmarkPopover(QWidget* parent);

    enum class JustAdded : u8 {
        No,
        Yes,
    };
    void set_bookmark(String id, JustAdded);
    void update_chrome_style(QPalette const&);

    // Removes the bookmark, by calling the given function.
    Function<void(Function<void()> const& remove)> on_remove;

private:
    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;

    void save();

    String m_bookmark_id;
    QLabel* m_title_label { nullptr };
    QLineEdit* m_name_field { nullptr };
    QComboBox* m_folder_picker { nullptr };
    QWidget* m_folder_row { nullptr };
    bool m_is_removed { false };
};

}
