// META: title=`Origin.from(Location)`
// META: script=/common/get-host-info.sub.js

test(t => {
  assert_throws_js(TypeError, _ => Origin.from(window.location));
}, `Origin.from(window.location) throws.`);

async_test(t => {
  const el = document.createElement('iframe');
  el.src = get_host_info().REMOTE_ORIGIN + "/common/blank.html";
  el.onload = t.step_func_done(_ => {
    assert_throws_js(TypeError, _ => Origin.from(el.contentWindow.location));
  });
  document.body.appendChild(el);
}, `Origin.from(Location) throws for cross-origin frames.`);
