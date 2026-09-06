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
#include <LibCore/Directory.h>
#include <LibCore/Process.h>
#include <LibWebView/CrashReport.h>
#include <dlfcn.h>

namespace WebView {

using namespace Core::CrashReportData;

static ByteString symbolicate_image(ReportFrame const& frame, Image const& image)
{
    // Match binary identities before translating between processes' ASLR slides.
    if (frame.binary.size != image.binary.size || frame.binary.bytes != image.binary.bytes)
        return {};
    if (frame.address < image.start - image.relocation || frame.address >= image.end - image.relocation)
        return {};
    auto address = frame.address + image.relocation;

    Dl_info info_for_address {};
    if (!dladdr(reinterpret_cast<void const*>(address), &info_for_address) || !info_for_address.dli_sname)
        return {};
    auto symbol = demangle(StringView { info_for_address.dli_sname, strlen(info_for_address.dli_sname) });
    if (sanitize_backtrace_symbol(symbol).is_empty())
        return {};
    return ByteString::formatted("{} + {:#x}", symbol, address - reinterpret_cast<FlatPtr>(info_for_address.dli_saddr));
}

ByteString CrashReport::symbolicate_frame(ReportFrame const& frame)
{
#if defined(AK_OS_MACOS)
    for (u32 i = 0; i < _dyld_image_count(); ++i) {
        auto symbol = symbolicate_image(frame, image_at_index(i));
        if (!symbol.is_empty())
            return symbol;
    }
    return {};
#else
    struct Context {
        ReportFrame const& frame;
        ByteString symbol;
    } context { frame, {} };
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* opaque_context) {
        auto& context = *static_cast<Context*>(opaque_context);
        context.symbol = symbolicate_image(context.frame, image_from_phdr_info(*info));
        return context.symbol.is_empty() ? 0 : 1;
    },
        &context);
    return context.symbol;
#endif
}

bool CrashReport::is_supported()
{
    return true;
}

ErrorOr<void> CrashReport::show_directory()
{
    TRY(Core::Directory::create(directory(), Core::Directory::CreateDirectories::Yes, 0700));
    Vector<ByteString> arguments { directory() };
#if defined(AK_OS_MACOS)
    TRY(Core::Process::spawn("/usr/bin/open"sv, arguments));
#else
    TRY(Core::Process::spawn({
        .executable = "xdg-open"sv,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    }));
#endif
    return {};
}

}
