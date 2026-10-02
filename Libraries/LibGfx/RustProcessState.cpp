/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/Mutex.h>
#include <AK/Singleton.h>
#include <AK/Types.h>
#include <AK/Vector.h>

// The process-wide state the Rust graphics crate needs exactly one copy of.
//
// `libgfx_rust` is compiled into more than one library: LibGfx links it, and the Rust crates of
// LibWeb and LibCompositing depend on it, so its code and every `static` in it end up in each of
// them. A dynamic linker with a flat namespace binds most calls to one copy and the duplication
// stays invisible; one with two-level namespaces, as macOS has, gives each library its own copy of
// every `static`, and LibCompositing hides its copy's symbols everywhere. A counter one library
// increments is then not the counter the other reads, and nothing about the Rust says so.
//
// So the crate keeps no process-wide state of its own. What has to be shared lives here, in a
// library there is only one of, and the crate reaches it through `extern "C"` entry points. The
// same goes for LibCompositing's crate, which LibWeb's crate depends on.

namespace Gfx {

// NB: Declared in the namespace the definitions below are in, so each has a previous declaration.
extern "C" {
u64 ladybird_gfx_process_next_path_identity();
u64 ladybird_gfx_process_next_structural_epoch();
void ladybird_gfx_process_note_crate_copy(void const* marker);
size_t ladybird_gfx_process_crate_copies_seen();
void ladybird_gfx_process_note_wanted_pending_face(u64 face_id);
void ladybird_gfx_process_requeue_wanted_pending_face(u64 face_id);
void ladybird_gfx_process_take_wanted_pending_faces(void* context, void (*visit)(void*, u64, bool));
}

namespace {

Atomic<u64> s_next_path_identity { 1 };

// LibCompositing's crate is compiled into more than one library too, and it numbers the visual
// context trees it builds with these.
Atomic<u64> s_next_structural_epoch { 1 };

// The marker of every copy of the crate that has reported itself. There are three at most:
// LibGfx's, LibWeb's and LibCompositing's.
Array<Atomic<FlatPtr>, 3> s_crate_copies;

// The pending web font faces render passes wanted while looking code points up in frozen cascades, each with whether
// it was offered to the document once already, until the document requests their loads.
struct WantedPendingFace {
    u64 id { 0 };
    bool has_been_retried { false };
};

struct WantedPendingFaces {
    AK_ALLOC_WITH_KMALLOC;

    Mutex mutex;
    Vector<WantedPendingFace> faces;
};

Singleton<WantedPendingFaces> s_wanted_pending_faces;

}

extern "C" u64 ladybird_gfx_process_next_path_identity()
{
    return s_next_path_identity.fetch_add(1, AK::memory_order_relaxed);
}

extern "C" u64 ladybird_gfx_process_next_structural_epoch()
{
    return s_next_structural_epoch.fetch_add(1, AK::memory_order_relaxed);
}

extern "C" void ladybird_gfx_process_note_crate_copy(void const* marker)
{
    auto address = bit_cast<FlatPtr>(marker);
    for (auto& copy : s_crate_copies) {
        FlatPtr expected = 0;
        if (copy.compare_exchange_strong(expected, address, AK::memory_order_relaxed) || expected == address)
            return;
    }
    VERIFY_NOT_REACHED();
}

// Reachable by name, so that a test can ask how many copies have reported themselves through each
// library's own copy of the crate, and so prove there is one store.
extern "C" size_t ladybird_gfx_process_crate_copies_seen()
{
    size_t seen = 0;
    for (auto& copy : s_crate_copies) {
        if (copy.load(AK::memory_order_relaxed) != 0)
            ++seen;
    }
    return seen;
}

extern "C" void ladybird_gfx_process_note_wanted_pending_face(u64 face_id)
{
    MutexLocker locker(s_wanted_pending_faces->mutex);
    s_wanted_pending_faces->faces.append({ face_id, false });
}

extern "C" void ladybird_gfx_process_requeue_wanted_pending_face(u64 face_id)
{
    MutexLocker locker(s_wanted_pending_faces->mutex);
    s_wanted_pending_faces->faces.append({ face_id, true });
}

// Hands every wanted face to `visit` and forgets them. The visitor may want faces again, which go to the next call.
extern "C" void ladybird_gfx_process_take_wanted_pending_faces(void* context, void (*visit)(void*, u64, bool))
{
    Vector<WantedPendingFace> faces;
    {
        MutexLocker locker(s_wanted_pending_faces->mutex);
        faces = move(s_wanted_pending_faces->faces);
    }
    for (auto const& face : faces)
        visit(context, face.id, face.has_been_retried);
}

}
