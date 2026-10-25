// META: title=XMLHttpRequest: byte order marks split across chunks

// content-in-chunks.py sends each hex-encoded chunk on its own, so these check
// that decoding carries over from one chunk to the next.
function load(t, responseType, chunks) {
  return new Promise(resolve => {
    const client = new XMLHttpRequest();
    client.responseType = responseType;
    client.open("GET", "resources/content-in-chunks.py?type=text/plain" +
        chunks.map(chunk => "&chunk=" + chunk).join(""));
    client.onload = t.step_func(() => resolve(client));
    client.send();
  });
}

promise_test(async t => {
  const client = await load(t, "", ["ef", "bbbf50"]);
  assert_equals(client.responseText, "P");
}, "responseText strips a UTF-8 byte order mark split across chunks");

promise_test(async t => {
  const client = await load(t, "", ["ff", "fe5000"]);
  assert_equals(client.responseText, "P");
}, "responseText uses a UTF-16LE byte order mark split across chunks");

promise_test(async t => {
  const client = await load(t, "", ["50", "efbbbf"]);
  assert_equals(client.responseText, "P\ufeff");
}, "responseText does not strip a UTF-8 byte order mark in a later chunk");

promise_test(async t => {
  const client = await load(t, "json", ["ef", "bbbf31"]);
  assert_equals(client.response, 1);
}, "JSON response strips a UTF-8 byte order mark split across chunks");
