// META: script=/resources/testdriver.js?feature=bidi
// META: script=/resources/testdriver-vendor.js
"use strict";

function check_equals(original, json) {
  const proto = Object.getPrototypeOf(original);
  const keys = Object.keys(proto).filter(
    (k) => typeof original[k] !== "function",
  );
  for (const key of keys) {
    assert_equals(
      original[key],
      json[key],
      `${original.constructor.name} ${key} entry does not match its toJSON value`,
    );
  }
}

promise_setup(async () => {
  // Ensure permission is granted before proceeding.
  await test_driver.bidi.permissions.set_permission({
    descriptor: {name: "geolocation"},
    state: "granted",
  });
});

promise_test(async (t) => {
  t.add_cleanup(async () => {
    await test_driver.bidi.emulation.set_geolocation_override(
      {coordinates: null});
  });

  const latitude = 51.478;
  const longitude = -0.166;
  const accuracy = 100;
    await test_driver.bidi.emulation.set_geolocation_override({
      coordinates: {latitude, longitude, accuracy}
  });

  const position = await new Promise((resolve, reject) => {
    navigator.geolocation.getCurrentPosition(resolve, reject);
  });

  const json = position.toJSON();
  assert_equals(
    position.timestamp,
    json.timestamp,
    "GeolocationPosition timestamp entry does not match its toJSON value",
  );

  check_equals(position.coords, json.coords);
  check_equals(position.coords, position.coords.toJSON());
}, "Test toJSON() in GeolocationPosition and GeolocationCoordinates.");
