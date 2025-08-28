// Set permissions for camera and microphone using Web Driver
// Status can be one of "granted" or "denied"
// Scope take values from permission names
async function setMediaPermission(status="granted", scope=["camera", "microphone"]) {
  try {
    for (let s of scope) {
      await test_driver.set_permission({ name: s }, status);
    }
  } catch (e) {
    const noSetPermissionSupport = typeof e === "string" && e.match(/set_permission not implemented/);
    if (!(noSetPermissionSupport ||
          (e instanceof Error && e.message.match("unimplemented")) )) {
      throw e;
    }
    // Web Driver not implemented action
    // FF: https://bugzilla.mozilla.org/show_bug.cgi?id=1524074

    // with current WPT runners, will default to granted state for FF and Safari
    // throw if status!="granted" to invalidate test results
    if (status === "denied") {
      assert_implements_optional(!noSetPermissionSupport, "Unable to set permission to denied for this test");
    }
  }
}
