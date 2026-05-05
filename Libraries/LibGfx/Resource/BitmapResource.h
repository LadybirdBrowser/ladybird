/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Platform.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/BitmapInfo.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Resource/Resource.h>

namespace Gfx {

class DecodedImageFrame;

class BitmapRegistry {
public:
    BitmapRegistry();
    BitmapRegistry(BitmapRegistry&&);
    BitmapRegistry& operator=(BitmapRegistry&&);
    ~BitmapRegistry();

    Optional<ResourceID> register_bitmap(ResourceID resource_id, DecodedImageFrame const&);
    Optional<BitmapInfo> description_for(ResourceID resource_id) const { return m_last_infos.get(resource_id); }
    Vector<ResourceTransfer> take_pending_transfers() { return move(m_pending_transfers); }
    void invalidate_resource(ResourceID resource_id);
    void clear();

private:
    ErrorOr<SharedImage*> ensure_shared_image(ResourceID resource_id, DecodedImageFrame const&);

    HashMap<ResourceID, OwnPtr<SharedImage>> m_shared_images;
    HashTable<ResourceID> m_resources;
    Vector<ResourceTransfer> m_pending_transfers;
    HashMap<ResourceID, BitmapInfo> m_last_infos;
};

}
