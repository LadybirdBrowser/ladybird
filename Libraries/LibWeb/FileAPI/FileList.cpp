/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/FileAPI/FileList.h>

namespace Web::FileAPI {

extern "C" {

// Defined in Bindings/Rust/src/file_api/exports.rs.
void* web_file_list_create(JSVM*);
void web_file_list_add_file(JSVM*, void* list, void* file);
u32 web_file_list_length(void* list);
void* web_file_list_item(void* list, u32 index);
}

GC::Ref<FileList> FileList::create()
{
    return GC::Ref { *static_cast<FileList*>(web_file_list_create(JS::HostABI::vm_to_abi(JS::VM::the()))) };
}

void FileList::add_file(GC::Ref<File> file)
{
    web_file_list_add_file(JS::HostABI::vm_to_abi(JS::VM::the()), this, file.ptr());
}

WebIDL::UnsignedLong FileList::length() const
{
    return web_file_list_length(const_cast<FileList*>(this));
}

GC::Ptr<File> FileList::item(size_t index) const
{
    if (index > NumericLimits<u32>::max())
        return {};
    return static_cast<File*>(web_file_list_item(const_cast<FileList*>(this), index));
}

}
