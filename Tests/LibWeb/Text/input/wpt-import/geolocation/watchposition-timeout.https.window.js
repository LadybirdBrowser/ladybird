// META: script=/resources/testdriver.js
// META: script=/resources/testdriver-vendor.js

promise_setup(async () => {
  await test_driver.set_permission({ name: "geolocation" }, "granted");
});

promise_test(async (t) => {
  let timeoutCount = 0;

  // This may still succeed without timeout in case there's a cache.
  const watchId = navigator.geolocation.watchPosition(() => {}, (error) => {
    if (error.code === GeolocationPositionError.TIMEOUT) {
      ++timeoutCount;
    }
  }, { timeout: 1 });
  t.add_cleanup(() => navigator.geolocation.clearWatch(watchId));

  await new Promise(r => setTimeout(r, 100));

  assert_true(timeoutCount < 2, "At most one timeout should have been seen");
}, "Passing timeout=1 should not cause multiple timeout errors");
