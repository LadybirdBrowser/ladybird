// Try explicitly denying so that persist() won't wait for user prompt
async function tryDenyingPermission() {
  try {
    await test_driver.set_permission({ name: "persistent-storage" }, "denied");
  } catch {
    // Not all implementations support this yet, but some implementations may
    // still be able to continue without explicit permission
  }
}
