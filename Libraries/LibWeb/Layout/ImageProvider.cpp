/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/ImageProvider.h>

namespace Web::Layout {

void ImageProvider::did_update_alt_text(ImageBox& layout_node)
{
    layout_node.dom_node_did_update_alt_text({});
}

}
