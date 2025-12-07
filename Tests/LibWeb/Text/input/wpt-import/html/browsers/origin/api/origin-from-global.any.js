// META: title=`Origin.from(WindowOrWorkerGlobalScope)`
// META: global=window,worker
// META: script=/common/get-host-info.sub.js

test(t => {
  const origin = Origin.from(globalThis);
  assert_true(!!origin);
  assert_false(origin.opaque, "Origin should not be opaque.");
  assert_true(origin.isSameOrigin(Origin.from(get_host_info().ORIGIN)));
}, `Origin.from(globalThis) is a tuple origin.`);
