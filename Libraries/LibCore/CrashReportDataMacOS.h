/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/CrashReportData.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/vm_prot.h>

namespace Core::CrashReportData {

inline Image image_at_index(u32 index)
{
    Image image;
    auto const* header = _dyld_get_image_header(index);
    if (header->magic != MH_MAGIC_64)
        return image;
    auto const* command = reinterpret_cast<load_command const*>(reinterpret_cast<u8 const*>(header) + sizeof(mach_header_64));
    image.relocation = _dyld_get_image_vmaddr_slide(index);
    for (u32 j = 0; j < header->ncmds; ++j) {
        if (command->cmd == LC_UUID) {
            auto const* uuid = reinterpret_cast<uuid_command const*>(command);
            image.binary.size = 16;
            __builtin_memcpy(image.binary.bytes.data(), uuid->uuid, 16);
        } else if (command->cmd == LC_SEGMENT_64) {
            auto const* segment = reinterpret_cast<segment_command_64 const*>(command);
            if (segment->initprot & VM_PROT_EXECUTE) {
                auto start = segment->vmaddr + image.relocation;
                auto end = start + segment->vmsize;
                image.start = image.start ? min(image.start, start) : start;
                image.end = max(image.end, end);
            }
        }
        command = reinterpret_cast<load_command const*>(reinterpret_cast<u8 const*>(command) + command->cmdsize);
    }
    return image;
}

}
