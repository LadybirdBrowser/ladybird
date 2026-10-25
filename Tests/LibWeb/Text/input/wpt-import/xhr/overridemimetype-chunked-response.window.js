// The response is delivered one byte-sequence at a time, with multi-byte
// characters split across chunk boundaries, so the final encoding has to be
// resolved once and then kept for the whole body.
function chunkedURL(type, chunks) {
  return "resources/content-in-chunks.py?type=" + encodeURIComponent(type) +
      chunks.map(chunk => "&chunk=" + chunk).join("");
}

// "テスト" (83 65 83 58 83 67) in Shift_JIS, with the second and third
// characters' bytes split across chunk boundaries.
const shiftJISChunks = ["8365", "83", "5883", "67"];

async_test(t => {
  const client = new XMLHttpRequest();
  client.onload = t.step_func_done(() => {
    assert_equals(client.responseText, "テスト");
  });
  client.open("GET", chunkedURL("text/plain", shiftJISChunks));
  client.overrideMimeType("text/plain;charset=Shift_JIS");
  client.send();
}, "overrideMimeType() charset is used for every chunk of a chunked response");

async_test(t => {
  const client = new XMLHttpRequest();
  client.onload = t.step_func_done(() => {
    assert_equals(client.responseText, "テスト");
  });
  client.open("GET", chunkedURL("text/plain;charset=UTF-8", shiftJISChunks));
  client.overrideMimeType("text/plain;charset=Shift_JIS");
  client.send();
}, "overrideMimeType() charset overrides the response charset for every chunk of a chunked response");

async_test(t => {
  const client = new XMLHttpRequest();
  client.onload = t.step_func_done(() => {
    // The override has no charset of its own, so the response charset has to
    // survive for the whole body.
    assert_equals(client.responseText, "テスト");
  });
  client.open("GET", chunkedURL("text/plain;charset=Shift_JIS", shiftJISChunks));
  client.overrideMimeType("text/xml");
  client.send();
}, "Response charset is used for every chunk of a chunked response when overrideMimeType() has no charset");

async_test(t => {
  const client = new XMLHttpRequest();
  client.onload = t.step_func_done(() => {
    assert_equals(client.responseText, "aéb");
  });
  // "é" in UTF-8, split across a chunk boundary.
  client.open("GET", chunkedURL("text/plain;charset=UTF-8", ["61C3", "A962"]));
  client.send();
}, "Response charset is used for every chunk of a chunked response");
