// META: title=`Origin.from(MessageEvent)` from opaque origins.
// META: script=/common/get-host-info.sub.js

async_test(t => {
  const el = document.createElement('iframe');
  el.sandbox = "allow-scripts";
  el.srcdoc = `<script>window.top.postMessage("Hi.", "*");<\/script>`;
  window.addEventListener("message", t.step_func(e => {
    if (e.source === el.contentWindow) {
      const origin = Origin.from(e);
      assert_true(!!origin);
      assert_true(origin.opaque);
      t.done();
    }
  }));
  document.body.appendChild(el);
}, `Origin.from(MessageEvent) returns an opaque origin for a sandboxed frame.`);

async_test(t => {
  const el = document.createElement('iframe');
  el.sandbox = "allow-scripts";
  el.srcdoc = `
    <script>
      window.top.postMessage("Hi.", "*");
      window.top.postMessage("Bye.", "*");
    <\/script>`;
  let eventOrigin = null;
  window.addEventListener("message", t.step_func(e => {
    if (e.source === el.contentWindow) {
      const origin = Origin.from(e);
      assert_true(!!origin);
      assert_true(origin.opaque);
      if (eventOrigin) {
        assert_true(eventOrigin.isSameOrigin(origin));
        t.done();
      } else {
        eventOrigin = origin;
      }
    }
  }));
  document.body.appendChild(el);
}, `Origin.from(MessageEvent) returns the same opaque origin for each message from a sandboxed frame.`);

async_test(t => {
  const el = document.createElement('iframe');
  el.sandbox = "allow-scripts";
  el.srcdoc = `
    <script>
      window.top.postMessage("Hi.", "*");
      window.addEventListener("message", e => { navigation.reload(); });
    <\/script>`;
  let eventOrigin = null;
  window.addEventListener("message", t.step_func(e => {
    if (e.source === el.contentWindow) {
      const origin = Origin.from(e);
      assert_true(!!origin);
      assert_true(origin.opaque);
      if (eventOrigin) {
        assert_false(eventOrigin.isSameOrigin(origin));
        t.done();
      } else {
        eventOrigin = origin;
        e.source.postMessage("Reload thyself.", "*");
      }
    }
  }));
  document.body.appendChild(el);
}, `Origin.from(MessageEvent) returns distinct opaque origins for each message from a reloaded sandboxed frame.`);
