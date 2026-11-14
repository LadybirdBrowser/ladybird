/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/MachPort.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibTest/TestCase.h>

TEST_CASE(a_shared_image_buffer_imports_its_own_exported_image)
{
    auto buffer = Gfx::SharedImageBuffer::create({ 4, 4 });
    auto imported = TRY_OR_FAIL(Gfx::SharedImageBuffer::import_from_shared_image(buffer.export_shared_image()));
    EXPECT_EQ(imported.bitmap()->size(), Gfx::IntSize(4, 4));
}

TEST_CASE(a_port_that_is_not_an_iosurface_fails_to_import)
{
    auto receive_right = TRY_OR_FAIL(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto send_right = TRY_OR_FAIL(receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));
    EXPECT(Gfx::SharedImageBuffer::import_from_shared_image(Gfx::SharedImage { move(send_right) }).is_error());
}
