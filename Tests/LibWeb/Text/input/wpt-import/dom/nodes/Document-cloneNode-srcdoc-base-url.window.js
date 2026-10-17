// See https://github.com/whatwg/dom/issues/454

function srcdocDocument(t, srcdoc) {
  return new Promise(resolve => {
    const iframe = document.createElement("iframe");
    iframe.srcdoc = srcdoc;
    iframe.onload = () => resolve(iframe);
    t.add_cleanup(() => iframe.remove());
    document.body.appendChild(iframe);
  });
}

promise_test(async t => {
  const doc = (await srcdocDocument(t, "<base href='sub/'>x")).contentDocument;
  assert_equals(doc.baseURI, new URL("sub/", document.baseURI).href);
}, "A relative <base> in an about:srcdoc document resolves against the inherited base URL");

promise_test(async t => {
  const doc = (await srcdocDocument(t, "<base href='sub/'>x")).contentDocument;
  const expected = new URL("sub/", document.baseURI).href;

  assert_equals(doc.cloneNode(true).baseURI, expected, "deep");
  assert_equals(doc.cloneNode(false).baseURI, expected, "shallow");
}, "Clone of an about:srcdoc document with a relative <base>");

promise_test(async t => {
  const doc = (await srcdocDocument(t, "x")).contentDocument;

  const shallow = doc.cloneNode(false);
  const base = shallow.createElement("base");
  base.setAttribute("href", "sub/");
  shallow.appendChild(base);
  assert_equals(shallow.baseURI, new URL("sub/", document.baseURI).href);
}, "A relative <base> inserted into a shallow clone of an about:srcdoc document resolves against the inherited base URL");

promise_test(async t => {
  const doc = (await srcdocDocument(t, "<base href='https://example.com/'>x")).contentDocument;
  const deep = doc.cloneNode(true);
  assert_equals(deep.baseURI, "https://example.com/", "deep clone before");

  for (const d of [doc, deep]) {
    d.querySelector("base").setAttribute("href", "data:/,test");
  }
  assert_equals(doc.baseURI, document.baseURI, "source");
  assert_equals(deep.baseURI, document.baseURI, "deep clone");
}, "A data: <base> in an about:srcdoc document and its clone falls back to the inherited base URL");
