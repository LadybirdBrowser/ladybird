// See https://github.com/whatwg/dom/issues/454

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
  const expected = new URL("sub/", doc.baseURI).href;

  const base = doc.createElement("base");
  base.setAttribute("href", "sub/");
  doc.head.appendChild(base);
  assert_equals(doc.baseURI, expected);
}, "A relative <base> in an about:blank document resolves against the inherited base URL");

promise_test(async t => {
  const doc = (await aboutBlankDocument(t)).contentDocument;
  const expected = new URL("sub/", doc.baseURI).href;

  const base = doc.createElement("base");
  base.setAttribute("href", "sub/");
  doc.head.appendChild(base);

  assert_equals(doc.cloneNode(true).baseURI, expected, "deep");
  assert_equals(doc.cloneNode(false).baseURI, expected, "shallow");
}, "Clone of an about:blank document with a relative <base>");

promise_test(async t => {
  const doc = (await aboutBlankDocument(t)).contentDocument;
  const expected = new URL("sub/", doc.baseURI).href;

  const shallow = doc.cloneNode(false);
  const base = shallow.createElement("base");
  base.setAttribute("href", "sub/");
  shallow.appendChild(base);
  assert_equals(shallow.baseURI, expected);
}, "A relative <base> inserted into a shallow clone of an about:blank document resolves against the inherited base URL");

promise_test(async t => {
  const doc = (await aboutBlankDocument(t)).contentDocument;
  const inherited = doc.baseURI;

  const base = doc.createElement("base");
  base.setAttribute("href", "https://example.com/");
  doc.head.appendChild(base);
  const deep = doc.cloneNode(true);
  assert_equals(deep.baseURI, "https://example.com/", "deep clone before");

  for (const d of [doc, deep]) {
    d.querySelector("base").setAttribute("href", "data:/,test");
  }
  assert_equals(doc.baseURI, inherited, "source");
  assert_equals(deep.baseURI, inherited, "deep clone");
}, "A data: <base> in an about:blank document and its clone falls back to the inherited base URL");
