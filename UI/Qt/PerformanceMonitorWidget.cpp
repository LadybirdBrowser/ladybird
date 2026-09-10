/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibWebView/TabPerformanceStats.h>
#include <QPainter>
#include <QPainterPath>
#include <UI/Qt/PerformanceMonitorWidget.h>
#include <UI/Qt/StringUtils.h>

namespace Ladybird {

static constexpr Array<char const*, 5> s_labels { "CPU ", "MEM ", "↓ ", "↑ ", "FPS " };
static constexpr Array<char const*, 5> s_value_templates { "9999%", "999.9 MB", "999.9 MB/s", "999.9 MB/s", "999" };
static constexpr int metric_spacing = 8;
static constexpr int text_start = 48;
static constexpr int trailing_padding = 4;

PerformanceMonitorWidget::PerformanceMonitorWidget(QWidget* parent)
    : QWidget(parent)
{
    auto text_font = font();
    text_font.setPointSizeF(10);
    text_font.setFeature("tnum", 1);
    setFont(text_font);
    setFixedSize(sizeHint());
}

QSize PerformanceMonitorWidget::sizeHint() const
{
    auto width = text_start + trailing_padding + metric_spacing * (s_labels.size() - 1);
    for (size_t i = 0; i < s_labels.size(); ++i)
        width += fontMetrics().horizontalAdvance(s_labels[i]) + fontMetrics().horizontalAdvance(s_value_templates[i]);
    return { static_cast<int>(width), 24 };
}

void PerformanceMonitorWidget::set_stats(WebView::TabPerformanceStats const& stats)
{
    m_stats = stats;
    update();
}

QStringList PerformanceMonitorWidget::text_segments() const
{
    return {
        m_stats.cpu_percent.has_value() ? QString("%1%").arg(*m_stats.cpu_percent, 0, 'f', 0) : QString("—"),
        m_stats.memory_bytes.has_value() ? qstring_from_ak_string(WebView::format_performance_bytes(*m_stats.memory_bytes)) : QString("—"),
        QString("%1/s").arg(qstring_from_ak_string(WebView::format_performance_bytes(m_stats.download_bytes_per_second))),
        QString("%1/s").arg(qstring_from_ak_string(WebView::format_performance_bytes(m_stats.upload_bytes_per_second))),
        m_stats.frames_per_second.has_value() ? QString("%1").arg(*m_stats.frames_per_second, 0, 'f', 0) : QString("—"),
    };
}

void PerformanceMonitorWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setFont(font());
    auto neutral = palette().color(QPalette::WindowText);
    auto label_color = neutral;
    label_color.setAlphaF(neutral.alphaF() * 0.65);
    auto x = text_start;
    QString accessible_text;
    auto segments = text_segments();
    Array<QColor, 5> colors { neutral, neutral, QColor(40, 135, 220), QColor(210, 130, 35), neutral };
    for (size_t i = 0; i < s_labels.size(); ++i) {
        QString label = s_labels[i];
        auto label_width = fontMetrics().horizontalAdvance(label);
        auto value_width = fontMetrics().horizontalAdvance(s_value_templates[i]);
        painter.setPen(i == 2 || i == 3 ? colors[i] : label_color);
        painter.drawText(QRect(x, 0, label_width, height()), Qt::AlignVCenter | Qt::AlignLeft, label);
        painter.setPen(colors[i]);
        painter.save();
        painter.translate(x + label_width, 0);
        auto actual_width = fontMetrics().horizontalAdvance(segments[i]);
        // NB: Very large counters fit inside their column without moving neighboring metrics.
        if (actual_width > value_width) {
            painter.scale(static_cast<double>(value_width) / actual_width, 1);
            painter.drawText(QRect(0, 0, actual_width, height()), Qt::AlignVCenter | Qt::AlignLeft, segments[i]);
        } else {
            painter.drawText(QRect(0, 0, value_width, height()), Qt::AlignVCenter | Qt::AlignLeft, segments[i]);
        }
        painter.restore();
        x += label_width + value_width + metric_spacing;
        accessible_text += label + segments[i] + ' ';
    }
    setAccessibleName(accessible_text);
    if (m_stats.cpu_history.size() < 2)
        return;
    double ceiling = 100;
    for (auto value : m_stats.cpu_history)
        ceiling = max(ceiling, value);
    QPainterPath path;
    for (size_t i = 0; i < m_stats.cpu_history.size(); ++i) {
        QPointF point(2 + 40.0 * i / (m_stats.cpu_history.size() - 1), 19 - 14 * m_stats.cpu_history[i] / ceiling);
        if (i == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    auto sparkline_color = neutral;
    sparkline_color.setAlphaF(neutral.alphaF() * 0.45);
    painter.setPen(QPen(sparkline_color, 0.75));
    painter.drawPath(path);
}

}
