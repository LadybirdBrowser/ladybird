/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <CameraServer/CameraBackend.h>
#include <LibTest/TestCase.h>

using namespace CameraServer;

static Vector<Camera::DeviceInfo> two_cameras()
{
    return {
        Camera::DeviceInfo { .dom_device_id = "one", .label = "The first camera", .group_id = {}, .is_default = true },
        Camera::DeviceInfo { .dom_device_id = "two", .label = "The second camera", .group_id = {}, .is_default = false },
    };
}

TEST_CASE(the_preferred_camera_is_the_default_one)
{
    auto devices = with_preferred_default(two_cameras(), "two");
    VERIFY(devices.size() == 2);
    EXPECT_EQ(devices[0].dom_device_id, "one");
    EXPECT(!devices[0].is_default);
    EXPECT_EQ(devices[1].dom_device_id, "two");
    EXPECT(devices[1].is_default);
}

TEST_CASE(the_default_stays_without_a_preference)
{
    auto devices = with_preferred_default(two_cameras(), {});
    VERIFY(devices.size() == 2);
    EXPECT(devices[0].is_default);
    EXPECT(!devices[1].is_default);
}

TEST_CASE(the_default_stays_while_the_preferred_camera_is_gone)
{
    auto devices = with_preferred_default(two_cameras(), "three");
    VERIFY(devices.size() == 2);
    EXPECT(devices[0].is_default);
    EXPECT(!devices[1].is_default);
}
