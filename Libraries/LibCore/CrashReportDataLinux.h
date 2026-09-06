/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/CrashReportData.h>
#include <elf.h>
#include <link.h>

namespace Core::CrashReportData {

inline void read_build_id(dl_phdr_info const& info, Image& image)
{
    for (size_t i = 0; i < info.dlpi_phnum; ++i) {
        auto const& program_header = info.dlpi_phdr[i];
        if (program_header.p_type != PT_NOTE)
            continue;

        auto const* note = reinterpret_cast<u8 const*>(info.dlpi_addr + program_header.p_vaddr);
        size_t remaining = program_header.p_memsz;
        while (remaining >= sizeof(ElfW(Nhdr))) {
            auto const& header = *reinterpret_cast<ElfW(Nhdr) const*>(note);
            auto name_size = (header.n_namesz + 3) & ~3u;
            auto description_size = (header.n_descsz + 3) & ~3u;
            if (name_size > remaining - sizeof(header) || description_size > remaining - sizeof(header) - name_size)
                break;

            auto const* name = note + sizeof(header);
            auto const* description = name + name_size;
            if (header.n_type == NT_GNU_BUILD_ID && header.n_namesz == 4 && __builtin_memcmp(name, "GNU", 4) == 0) {
                image.binary.size = min(header.n_descsz, image.binary.bytes.size());
                __builtin_memcpy(image.binary.bytes.data(), description, image.binary.size);
                return;
            }

            auto note_size = sizeof(header) + name_size + description_size;
            note += note_size;
            remaining -= note_size;
        }
    }
}

inline Image image_from_phdr_info(dl_phdr_info const& info)
{
    Image image;
    image.relocation = info.dlpi_addr;
    for (size_t i = 0; i < info.dlpi_phnum; ++i) {
        auto const& program_header = info.dlpi_phdr[i];
        if (program_header.p_type != PT_LOAD || !(program_header.p_flags & PF_X))
            continue;
        auto start = info.dlpi_addr + program_header.p_vaddr;
        auto end = start + program_header.p_memsz;
        image.start = image.start ? min(image.start, start) : start;
        image.end = max(image.end, end);
    }
    read_build_id(info, image);
    return image;
}

}
