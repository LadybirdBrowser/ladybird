/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/Noncopyable.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Types.h>

#include <fcntl.h>
#include <stdio.h>

#if !defined(AK_OS_WINDOWS)
#    include <sys/ioctl.h>
#    include <unistd.h>
#endif

namespace Test {

inline bool stdout_is_tty()
{
#if defined(AK_OS_WINDOWS)
    return false;
#else
    return isatty(STDOUT_FILENO);
#endif
}

inline size_t query_terminal_width(int fd, size_t fallback = 80)
{
#if !defined(AK_OS_WINDOWS)
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
#endif
    (void)fd;
    return fallback;
}

class LiveDisplay {
    AK_MAKE_NONCOPYABLE(LiveDisplay);
    AK_MAKE_NONMOVABLE(LiveDisplay);

public:
    enum Color : u8 {
        None,
        Red,
        Green,
        Yellow,
        Blue,
        Magenta,
        Cyan,
        Gray,
    };

    struct LabelColor {
        Color prefix;
        Color text;
    };

    struct Counter {
        StringView label;
        Color color;
        size_t value;
    };

    struct Options {
        size_t reserved_lines { 3 };
        // If non-empty, stdout and stderr are redirected to this file for the lifetime of the live display, and the display itself is written to the terminal.
        ByteString log_file_path;
    };

    class RenderTarget {
        AK_MAKE_NONCOPYABLE(RenderTarget);
        AK_MAKE_NONMOVABLE(RenderTarget);
        friend class LiveDisplay;

    public:
        StringBuilder& builder() { return m_builder; }
        size_t terminal_width() const { return m_terminal_width; }

        template<typename Callback>
        void line(Callback&& callback)
        {
            m_builder.append("\033[2K"sv);
            callback();
            m_builder.append('\n');
        }

        template<typename... Callbacks>
        void lines(Callbacks&&... callback)
        {
            (line(forward<Callbacks>(callback)), ...);
        }

        void label(StringView prefix, StringView text, LabelColor color = { Yellow, None })
        {
            m_builder.append(LiveDisplay::ansi_on(color.prefix));
            m_builder.append(prefix);
            m_builder.append(LiveDisplay::ansi_reset(color.prefix));

            size_t available = m_terminal_width > prefix.length() ? m_terminal_width - prefix.length() : 10;

            m_builder.append(LiveDisplay::ansi_on(color.text));
            if (text.length() > available && available > 3) {
                m_builder.append("..."sv);
                m_builder.append(text.substring_view(text.length() - available + 3));
            } else {
                m_builder.append(text);
            }
            m_builder.append(LiveDisplay::ansi_reset(color.text));
        }

        template<auto N>
        void counter(Counter const (&counters)[N])
        {
            bool first = true;
            for (auto const& c : counters) {
                if (!first)
                    m_builder.append(", "sv);
                first = false;
                m_builder.append(LiveDisplay::ansi_bold_on(c.color));
                m_builder.appendff("{}:", c.label);
                m_builder.append("\033[0m"sv);
                m_builder.appendff(" {}", c.value);
            }
        }

        void progress_bar(size_t completed, size_t total, StringView suffix = {})
        {
            auto counter_begin = m_builder.length();
            m_builder.appendff("{}/{} ", completed, total);
            if (!suffix.is_empty()) {
                m_builder.append(suffix);
                m_builder.append(' ');
            }
            size_t counter_length = m_builder.length() - counter_begin;
            size_t bar_width = m_terminal_width > counter_length + 3 ? m_terminal_width - counter_length - 3 : 20;
            size_t filled = total > 0 ? (completed * bar_width) / total : 0;
            size_t empty = bar_width > filled ? bar_width - filled : 0;

            m_builder.append("\033[32m["sv);
            for (size_t j = 0; j < filled; ++j)
                m_builder.append("█"sv);
            if (empty > 0 && filled < bar_width) {
                m_builder.append("\033[33m▓\033[0m\033[90m"sv);
                for (size_t j = 1; j < empty; ++j)
                    m_builder.append("░"sv);
            }
            m_builder.append("\033[32m]\033[0m"sv);
        }

    private:
        RenderTarget(StringBuilder& builder, size_t terminal_width)
            : m_builder(builder)
            , m_terminal_width(terminal_width)
        {
        }

        StringBuilder& m_builder;
        size_t m_terminal_width;
    };

    LiveDisplay() = default;
    ~LiveDisplay() { end(); }

    bool begin(Options options)
    {
#if defined(AK_OS_WINDOWS)
        (void)options;
        return false;
#else
        if (m_active)
            return false;

        m_reserved_lines = options.reserved_lines;
        m_log_file_path = move(options.log_file_path);

        if (!m_log_file_path.is_empty()) {
            int log_fd = ::open(m_log_file_path.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log_fd < 0)
                return false;

            m_saved_stdout_fd = dup(STDOUT_FILENO);
            m_saved_stderr_fd = dup(STDERR_FILENO);
            if (m_saved_stdout_fd < 0 || m_saved_stderr_fd < 0) {
                close(log_fd);
                return false;
            }

            (void)fflush(stdout);
            (void)fflush(stderr);
            (void)dup2(log_fd, STDOUT_FILENO);
            (void)dup2(log_fd, STDERR_FILENO);
            close(log_fd);

            int display_fd = dup(m_saved_stdout_fd);
            if (display_fd < 0)
                return false;
            m_output = fdopen(display_fd, "w");
            if (!m_output) {
                close(display_fd);
                return false;
            }
            (void)setvbuf(m_output, nullptr, _IONBF, 0);
        } else {
            m_output = stdout;
        }

        refresh_terminal_width();

        for (size_t i = 0; i < m_reserved_lines; ++i)
            (void)fputc('\n', m_output);
        (void)fflush(m_output);

        m_active = true;
        return true;
#endif
    }

    void end()
    {
#if !defined(AK_OS_WINDOWS)
        if (!m_active)
            return;

        clear();

        bool redirected = m_saved_stdout_fd >= 0 || m_saved_stderr_fd >= 0;
        if (redirected) {
            if (m_output) {
                (void)fclose(m_output);
                m_output = nullptr;
            }
            (void)fflush(stdout);
            (void)fflush(stderr);
            if (m_saved_stdout_fd >= 0) {
                (void)dup2(m_saved_stdout_fd, STDOUT_FILENO);
                close(m_saved_stdout_fd);
                m_saved_stdout_fd = -1;
            }
            if (m_saved_stderr_fd >= 0) {
                (void)dup2(m_saved_stderr_fd, STDERR_FILENO);
                close(m_saved_stderr_fd);
                m_saved_stderr_fd = -1;
            }
            (void)setvbuf(stdout, nullptr, _IOLBF, 0);
            (void)setvbuf(stderr, nullptr, _IONBF, 0);
        } else {
            m_output = nullptr;
        }
        m_active = false;
#endif
    }

    bool is_active() const { return m_active; }
    FILE* output() const { return m_output; }
    size_t terminal_width() const { return m_terminal_width; }
    size_t reserved_lines() const { return m_reserved_lines; }
    void set_reserved_lines(size_t n) { m_reserved_lines = n; }
    ByteString const& log_file_path() const { return m_log_file_path; }
    bool redirected_stdio() const { return m_saved_stdout_fd >= 0; }

    void refresh_terminal_width()
    {
#if !defined(AK_OS_WINDOWS)
        int fd = m_output ? fileno(m_output) : STDOUT_FILENO;
#else
        int fd = 0; // Not actually used.
#endif
        m_terminal_width = query_terminal_width(fd);
    }

    // Erase the reserved display area, leaving the cursor at the top of it.
    void clear()
    {
        if (!m_active || !m_output)
            return;
        StringBuilder builder;
        for (size_t i = 0; i < m_reserved_lines; ++i)
            builder.append("\033[A\r\033[2K"sv);
        write(builder.string_view());
    }

    template<typename Callback>
    void render(Callback&& callback)
    {
        if (!m_active || !m_output)
            return;

        StringBuilder builder;
        for (size_t i = 0; i < m_reserved_lines; ++i)
            builder.append("\033[A"sv);
        builder.append("\r"sv);

        RenderTarget target { builder, m_terminal_width };
        callback(target);

        write(builder.string_view());
    }

private:
    static constexpr StringView ansi_on(Color c)
    {
        switch (c) {
        case None:
            return ""sv;
        case Red:
            return "\033[31m"sv;
        case Green:
            return "\033[32m"sv;
        case Yellow:
            return "\033[33m"sv;
        case Blue:
            return "\033[34m"sv;
        case Magenta:
            return "\033[35m"sv;
        case Cyan:
            return "\033[36m"sv;
        case Gray:
            return "\033[90m"sv;
        }
        VERIFY_NOT_REACHED();
    }

    static constexpr StringView ansi_bold_on(Color c)
    {
        switch (c) {
        case None:
            return "\033[1m"sv;
        case Red:
            return "\033[1;31m"sv;
        case Green:
            return "\033[1;32m"sv;
        case Yellow:
            return "\033[1;33m"sv;
        case Blue:
            return "\033[1;34m"sv;
        case Magenta:
            return "\033[1;35m"sv;
        case Cyan:
            return "\033[1;36m"sv;
        case Gray:
            return "\033[1;90m"sv;
        }
        VERIFY_NOT_REACHED();
    }

    static constexpr StringView ansi_reset(Color c)
    {
        return c == None ? ""sv : "\033[0m"sv;
    }

    void write(StringView data)
    {
        (void)fwrite(data.characters_without_null_termination(), 1, data.length(), m_output);
        (void)fflush(m_output);
    }

    bool m_active { false };
    size_t m_reserved_lines { 0 };
    size_t m_terminal_width { 80 };
    FILE* m_output { nullptr };
    int m_saved_stdout_fd { -1 };
#if !defined(AK_OS_WINDOWS)
    int m_saved_stderr_fd { -1 };
#endif
    ByteString m_log_file_path;
};

}
