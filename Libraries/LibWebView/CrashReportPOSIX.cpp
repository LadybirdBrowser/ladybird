/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Demangle.h>
#if defined(AK_OS_MACOS)
#    include <LibCore/CrashReportDataMacOS.h>
#else
#    include <LibCore/CrashReportDataLinux.h>
#endif
#include <LibWebView/CrashReport.h>

namespace WebView {

using namespace Core::CrashReportData;

static Optional<SymbolAtAddress> symbolicate_image(ReportFrame const& frame, Image const& image)
{
    // Match binary identities before translating between processes' ASLR slides.
    if (frame.binary.size != image.binary.size || frame.binary.bytes != image.binary.bytes)
        return {};
    if (frame.address < image.start - image.relocation || frame.address >= image.end - image.relocation)
        return {};
    return symbol_at_address(frame.address + image.relocation);
}

void CrashReport::symbolicate_frame(ReportFrame& frame)
{
    Optional<SymbolAtAddress> symbol;
#if defined(AK_OS_MACOS)
    for (u32 i = 0; i < _dyld_image_count() && !symbol.has_value(); ++i)
        symbol = symbolicate_image(frame, image_at_index(i));
#else
    struct Context {
        ReportFrame const& frame;
        Optional<SymbolAtAddress>& symbol;
    } context { frame, symbol };
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* opaque_context) {
        auto& context = *static_cast<Context*>(opaque_context);
        context.symbol = symbolicate_image(context.frame, image_from_phdr_info(*info));
        return context.symbol.has_value() ? 1 : 0;
    },
        &context);
#endif
    if (symbol.has_value())
        describe_backtrace_frame(frame, { 0, ByteString::formatted("{} + {:#x}", symbol->name, symbol->offset), {}, 0, 0, false });
}

bool CrashReport::is_supported()
{
    return true;
}

}
