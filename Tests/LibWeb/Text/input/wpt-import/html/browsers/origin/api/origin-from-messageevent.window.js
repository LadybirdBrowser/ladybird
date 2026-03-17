// META: title=`Origin.from(MessageEvent)`
// META: script=/common/get-host-info.sub.js

console.log("hi")
test(t => {
  const e = new MessageEvent("message", { origin: get_host_info().ORIGIN });
  assert_throws_js(TypeError, _ => Origin.from(e));
}, "Constructed `MessageEvent` objects have no real origins.");

async_test(t => {
  const el = document.createElement('iframe');
  el.src = "/html/browsers/windows/resources/message-parent.html"
  window.addEventListener("message", t.step_func(e => {
    if (e.source === el.contentWindow) {
      const origin = Origin.from(e);
      assert_true(!!origin);
      assert_false(origin.opaque);
      assert_true(origin.isSameOrigin(Origin.from(get_host_info().ORIGIN)));
      t.done();
    }
  }));
  document.body.appendChild(el);
}, `Origin.from(MessageEvent) returns a tuple origin for messages from same-origin frames.`);

async_test(t => {
  const el = document.createElement('iframe');
  el.src = get_host_info().REMOTE_ORIGIN + "/html/browsers/windows/resources/message-parent.html"
  window.addEventListener("message", t.step_func(e => {
    if (e.source === el.contentWindow) {
      const origin = Origin.from(e);
      assert_true(!!origin);
      assert_false(origin.opaque);
      assert_true(origin.isSameOrigin(Origin.from(get_host_info().REMOTE_ORIGIN)));
      t.done();
    }
  }));
  document.body.appendChild(el);
}, `Origin.from(MessageEvent) returns a tuple origin for messages from cross-origin frames.`);

async_test(t => {
  const el = document.createElement('iframe');
  el.src = get_host_info().REMOTE_ORIGIN + "/html/browsers/windows/resources/message-parent.html"
  el.sandbox = "allow-scripts";
  window.addEventListener("message", t.step_func(e => {
    if (e.source === el.contentWindow) {
      const origin = Origin.from(e);
      assert_true(!!origin);
      assert_true(origin.opaque);
      assert_false(origin.isSameOrigin(Origin.from(get_host_info().REMOTE_ORIGIN)));
      t.done();
    }
  }));
  document.body.appendChild(el);
}, `Origin.from(MessageEvent) returns an opaque origin for messages from sandboxed frames.`);

async_test(t => {
  const w = window.open("/html/browsers/windows/resources/post-to-opener.html");
  window.addEventListener("message", t.step_func(e => {
    if (e.source === w) {
      const origin = Origin.from(e);
      assert_true(!!origin);
      assert_false(origin.opaque);
      assert_true(origin.isSameOrigin(Origin.from(get_host_info().ORIGIN)));
      t.done();
    }
  }));
}, `Origin.from(MessageEvent) returns a tuple origin for same-origin windows.`);

async_test(t => {
  const w = window.open(get_host_info().REMOTE_ORIGIN + "/html/browsers/windows/resources/post-to-opener.html");
  window.addEventListener("message", t.step_func(e => {
    if (e.source === w) {
      const origin = Origin.from(e);
      assert_true(!!origin);
      assert_false(origin.opaque);
      assert_true(origin.isSameOrigin(Origin.from(get_host_info().REMOTE_ORIGIN)));
      t.done();
    }
  }));
}, `Origin.from(MessageEvent) returns a tuple origin for cross-origin windows.`);
