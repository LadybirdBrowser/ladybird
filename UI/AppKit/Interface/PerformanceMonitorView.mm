/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Interface/PerformanceMonitorView.h>
#import <Utilities/Conversions.h>

static NSArray<NSString*>* labels() { return @[ @"CPU ", @"MEM ", @"↓ ", @"↑ ", @"FPS " ]; }
static NSArray<NSString*>* value_templates() { return @[ @"9999%", @"999.9 MB", @"999.9 MB/s", @"999.9 MB/s", @"999" ]; }
static NSFont* monitor_font() { return [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular]; }

@implementation PerformanceMonitorView
{
    WebView::TabPerformanceStats m_stats;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        [self setAccessibilityElement:YES];
        [self setAccessibilityRole:NSAccessibilityStaticTextRole];
    }
    return self;
}

- (NSSize)intrinsicContentSize
{
    CGFloat width = 48 + 4 + 8 * 4;
    auto* attributes = @{ NSFontAttributeName : monitor_font() };
    for (NSUInteger i = 0; i < labels().count; ++i)
        width += [labels()[i] sizeWithAttributes:attributes].width + [value_templates()[i] sizeWithAttributes:attributes].width;
    return NSMakeSize(ceil(width), 24);
}

- (void)updateStats:(WebView::TabPerformanceStats const&)stats
{
    m_stats = stats;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty_rect
{
    (void)dirty_rect;
    auto* neutral = NSColor.labelColor;
    auto* label_color = [neutral colorWithAlphaComponent:0.65];
    auto* font = monitor_font();
    auto* memory = m_stats.memory_bytes.has_value() ? Ladybird::string_to_ns_string(WebView::format_performance_bytes(*m_stats.memory_bytes)) : @"—";
    auto* values = @[
        m_stats.cpu_percent.has_value() ? [NSString stringWithFormat:@"%.0f%%", *m_stats.cpu_percent] : @"—",
        memory,
        [NSString stringWithFormat:@"%@/s", Ladybird::string_to_ns_string(WebView::format_performance_bytes(m_stats.download_bytes_per_second))],
        [NSString stringWithFormat:@"%@/s", Ladybird::string_to_ns_string(WebView::format_performance_bytes(m_stats.upload_bytes_per_second))],
        m_stats.frames_per_second.has_value() ? [NSString stringWithFormat:@"%.0f", *m_stats.frames_per_second] : @"—",
    ];
    auto* colors = @[ neutral, neutral, NSColor.systemBlueColor, NSColor.systemOrangeColor, neutral ];
    CGFloat x = 48;
    auto* accessible_text = [[NSMutableString alloc] init];
    for (NSUInteger i = 0; i < values.count; ++i) {
        auto* attributes = @{ NSFontAttributeName : font, NSForegroundColorAttributeName : colors[i] };
        auto label_size = [labels()[i] sizeWithAttributes:attributes];
        auto value_size = [values[i] sizeWithAttributes:attributes];
        auto value_width = [value_templates()[i] sizeWithAttributes:attributes].width;
        CGFloat y = (self.bounds.size.height - value_size.height) / 2;
        auto* label_attributes = @{ NSFontAttributeName : font, NSForegroundColorAttributeName : i == 2 || i == 3 ? colors[i] : label_color };
        [labels()[i] drawAtPoint:NSMakePoint(x, y) withAttributes:label_attributes];
        [NSGraphicsContext saveGraphicsState];
        auto* transform = [NSAffineTransform transform];
        [transform translateXBy:x + label_size.width yBy:y];
        if (value_size.width > value_width)
            [transform scaleXBy:value_width / value_size.width yBy:1];
        [transform concat];
        [values[i] drawAtPoint:NSZeroPoint withAttributes:attributes];
        [NSGraphicsContext restoreGraphicsState];
        x += label_size.width + value_width + 8;
        [accessible_text appendFormat:@"%@%@ ", labels()[i], values[i]];
    }
    [self setAccessibilityLabel:accessible_text];

    if (m_stats.cpu_history.size() < 2)
        return;
    double ceiling = 100;
    for (auto value : m_stats.cpu_history)
        ceiling = max(ceiling, value);
    auto* path = [NSBezierPath bezierPath];
    for (size_t i = 0; i < m_stats.cpu_history.size(); ++i) {
        auto point = NSMakePoint(2 + 40.0 * i / (m_stats.cpu_history.size() - 1), 5 + 14 * m_stats.cpu_history[i] / ceiling);
        if (i == 0)
            [path moveToPoint:point];
        else
            [path lineToPoint:point];
    }
    [[neutral colorWithAlphaComponent:0.45] setStroke];
    [path setLineWidth:0.75];
    [path stroke];
}
@end
