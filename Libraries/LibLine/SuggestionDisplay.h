/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibLine/StringMetrics.h>
#include <LibLine/SuggestionManager.h>

namespace Line {

class Editor;

class SuggestionDisplay {
public:
    virtual ~SuggestionDisplay() = default;
    virtual ErrorOr<void> display(SuggestionManager const&) = 0;
    virtual ErrorOr<bool> cleanup() = 0;
    virtual void finish() = 0;
    virtual void set_initial_prompt_lines(size_t) = 0;

    ErrorOr<void> redisplay(SuggestionManager const& manager, size_t lines, size_t columns)
    {
        if (m_is_showing_suggestions) {
            TRY(cleanup());
            set_vt_size(lines, columns);
            TRY(display(manager));
        } else {
            set_vt_size(lines, columns);
        }

        return {};
    }

    virtual void set_vt_size(size_t lines, size_t columns) = 0;

    size_t origin_row() const { return m_origin_row; }
    size_t origin_col() const { return m_origin_column; }

    void set_origin(int row, int col, Badge<Editor>)
    {
        m_origin_row = row;
        m_origin_column = col;
    }

protected:
    void did_display() { m_is_showing_suggestions = true; }
    void did_cleanup() { m_is_showing_suggestions = false; }

    int m_origin_row { 0 };
    int m_origin_column { 0 };
    bool m_is_showing_suggestions { false };
};

class XtermSuggestionDisplay : public SuggestionDisplay {
public:
    XtermSuggestionDisplay(size_t lines, size_t columns)
        : m_num_lines(lines)
        , m_num_columns(columns)
    {
    }
    virtual ~XtermSuggestionDisplay() override = default;
    virtual ErrorOr<void> display(SuggestionManager const&) override;
    virtual ErrorOr<bool> cleanup() override;
    virtual void finish() override
    {
        m_pages.clear();
    }

    virtual void set_initial_prompt_lines(size_t lines) override
    {
        m_prompt_lines_at_suggestion_initiation = lines;
    }

    virtual void set_vt_size(size_t lines, size_t columns) override
    {
        m_num_lines = lines;
        m_num_columns = columns;
        m_pages.clear();
    }

private:
    size_t fit_to_page_boundary(size_t selection_index);
    size_t m_lines_used_for_last_suggestions { 0 };
    size_t m_num_lines { 0 };
    size_t m_num_columns { 0 };
    size_t m_prompt_lines_at_suggestion_initiation { 0 };

    struct PageRange {
        size_t start;
        size_t end;
    };
    Vector<PageRange> m_pages;
};

}
