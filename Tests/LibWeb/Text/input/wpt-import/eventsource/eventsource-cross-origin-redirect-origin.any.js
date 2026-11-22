// META: title=EventSource: origin of the event stream's final URL
// META: script=/common/get-host-info.sub.js

async_test(t => {
  const crossOrigin = get_host_info().HTTP_REMOTE_ORIGIN;
  const target = crossOrigin + "/eventsource/resources/cors.py?run=message";
  const source = new EventSource(
    "/common/redirect.py?location=" + encodeURIComponent(target));
  t.add_cleanup(() => source.close());

  source.onerror = t.unreached_func("error event");
  source.onmessage = t.step_func_done(e => {
    assert_equals(e.data, "data", "data");
    assert_equals(e.origin, crossOrigin, "origin");
  });
}, "MessageEvent.origin is the origin of the final URL after a cross-origin redirect");
