// META: global=window,worker

const permissionNames = [
  // Powerful features in https://w3c.github.io/permissions/#registry-table-of-standardized-permissions
  "geolocation",
  "notifications",
  "push",

  // extra permissions not in there: https://github.com/w3c/permissions/issues/475
  "accelerometer",
  "background-fetch",
  "camera",
  "display-capture",
  "gyroscope",
  "local-network",
  "loopback-network",
  "magnetometer",
  "microphone",
  "midi",
  "nfc",
  "persistent-storage",
  "screen-wake-lock",
  "speaker-selection",
  "storage-access",
];

for (const permissionName of permissionNames) {
  promise_test(async () => {
    try {
      await navigator.permissions.query({ name: permissionName });
    } catch (e) {
      assert_equals(e.name, "TypeError", `${permissionName} can throw a TypeError if unsupported`);
    }
  }, `${permissionName} should be queried without crash`);
}
