// An about:blank document inherits its initiator's base URL. Cloning copies the
// computed base URL, so the clone reports the same value.
// See https://github.com/whatwg/dom/issues/454

const BASE_HREF = "http://example.com/foo/bar";

function aboutBlankDocument(t) {
  return new Promise(resolve => {
    const iframe = document.createElement("iframe");
    iframe.src = "about:blank";
    iframe.onload = () => resolve(iframe);
    t.add_cleanup(() => iframe.remove());
    document.body.appendChild(iframe);
  });
}

promise_test(async t => {
  const doc = (await aboutBlankDocument(t)).contentDocument;
  assert_equals(doc.URL, "about:blank", "URL");
  assert_equals(doc.baseURI, document.baseURI, "inherited base URL");
}, "An about:blank document inherits the initiator base URL");

promise_test(async t => {
  const doc = (await aboutBlankDocument(t)).contentDocument;
  const expected = doc.baseURI;

  const deep = doc.cloneNode(true);
  assert_equals(deep.URL, "about:blank", "URL");
  assert_equals(deep.baseURI, expected, "baseURI");
}, "Deep clone of an about:blank document");

promise_test(async t => {
  const doc = (await aboutBlankDocument(t)).contentDocument;
  const expected = doc.baseURI;

  const shallow = doc.cloneNode(false);
  assert_equals(shallow.URL, "about:blank", "URL");
  assert_equals(shallow.baseURI, expected, "baseURI");
}, "Shallow clone of an about:blank document");

// The inherited base URL is a snapshot, not a live lookup: the clone keeps it
// after the source iframe is detached.
promise_test(async t => {
  const iframe = await aboutBlankDocument(t);
  const doc = iframe.contentDocument;
  const expected = doc.baseURI;

  const shallow = doc.cloneNode(false);
  const deep = doc.cloneNode(true);
  iframe.remove();

  assert_equals(shallow.baseURI, expected, "shallow");
  assert_equals(deep.baseURI, expected, "deep");
}, "Clone of an about:blank document after detaching the source");

promise_test(async t => {
  const doc = (await aboutBlankDocument(t)).contentDocument;
  const base = doc.createElement("base");
  base.setAttribute("href", BASE_HREF);
  doc.head.appendChild(base);
  assert_equals(doc.baseURI, BASE_HREF, "source");

  assert_equals(doc.cloneNode(true).baseURI, BASE_HREF, "deep");
  assert_equals(doc.cloneNode(false).baseURI, BASE_HREF, "shallow");
}, "Clone of an about:blank document with a <base>");
