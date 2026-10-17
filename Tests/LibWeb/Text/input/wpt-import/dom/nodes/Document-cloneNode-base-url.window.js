// Cloning a document copies its computed base URL. A shallow clone therefore
// reports the same base URL as the source even though it has no <base> element.
// See https://github.com/whatwg/dom/issues/454

const BASE_HREF = "http://example.com/foo/bar";

test(() => {
  const doc = new DOMParser().parseFromString(
    `<!DOCTYPE html><base href="${BASE_HREF}">`, "text/html");
  assert_equals(doc.baseURI, BASE_HREF, "source");

  const deep = doc.cloneNode(true);
  assert_equals(deep.URL, doc.URL, "URL");
  assert_equals(deep.baseURI, BASE_HREF, "baseURI");
}, "Deep clone of a document with a <base>");

test(() => {
  const doc = new DOMParser().parseFromString(
    `<!DOCTYPE html><base href="${BASE_HREF}">`, "text/html");
  assert_equals(doc.baseURI, BASE_HREF, "source");

  const shallow = doc.cloneNode(false);
  assert_equals(shallow.URL, doc.URL, "URL");
  assert_equals(shallow.baseURI, BASE_HREF, "baseURI");
}, "Shallow clone of a document with a <base>");

test(() => {
  const doc = new DOMParser().parseFromString("<!DOCTYPE html>", "text/html");
  assert_equals(doc.baseURI, doc.URL, "source");

  assert_equals(doc.cloneNode(true).baseURI, doc.URL, "deep");
  assert_equals(doc.cloneNode(false).baseURI, doc.URL, "shallow");
}, "Clone of a document without a <base>");

test(() => {
  const doc = document.implementation.createHTMLDocument("");
  assert_equals(doc.URL, "about:blank", "URL");
  assert_equals(doc.baseURI, "about:blank", "source");

  assert_equals(doc.cloneNode(true).baseURI, "about:blank", "deep");
  assert_equals(doc.cloneNode(false).baseURI, "about:blank", "shallow");
}, "Clone of a createHTMLDocument() document without a <base>");

test(() => {
  const doc = document.implementation.createHTMLDocument("");
  const base = doc.createElement("base");
  base.setAttribute("href", BASE_HREF);
  doc.head.appendChild(base);
  assert_equals(doc.baseURI, BASE_HREF, "source");

  assert_equals(doc.cloneNode(true).baseURI, BASE_HREF, "deep");
  assert_equals(doc.cloneNode(false).baseURI, BASE_HREF, "shallow");
}, "Clone of a createHTMLDocument() document with a <base>");

// A relative <base> must resolve exactly once, against the copied URL, rather
// than a second time against the source's already-computed base URL.
test(() => {
  const doc = new DOMParser().parseFromString(
    `<!DOCTYPE html><base href="sub/">`, "text/html");
  const expected = new URL("sub/", doc.URL).href;
  assert_equals(doc.baseURI, expected, "source");
  assert_equals(doc.cloneNode(true).baseURI, expected, "baseURI");
}, "Deep clone of a document with a relative <base>");

test(() => {
  const doc = new DOMParser().parseFromString(
    `<!DOCTYPE html><base href="sub/">`, "text/html");
  const expected = new URL("sub/", doc.URL).href;
  assert_equals(doc.baseURI, expected, "source");
  assert_equals(doc.cloneNode(false).baseURI, expected, "baseURI");
}, "Shallow clone of a document with a relative <base>");

test(() => {
  const doc = new DOMParser().parseFromString(
    `<!DOCTYPE html><base href="${BASE_HREF}">`, "text/html");
  const shallow = doc.cloneNode(false);

  doc.querySelector("base").remove();
  assert_equals(doc.baseURI, doc.URL, "source updated");
  assert_equals(shallow.baseURI, BASE_HREF, "clone unchanged");
}, "Shallow clone is unaffected by later mutation of the source");

test(() => {
  const doc = new DOMParser().parseFromString(
    `<!DOCTYPE html><base href="${BASE_HREF}">`, "text/html");
  const shallow = doc.cloneNode(false);

  shallow.appendChild(shallow.createComment("mutate"));
  assert_equals(shallow.baseURI, BASE_HREF);
}, "Shallow clone's base URL survives later mutation of the clone");

test(() => {
  const doc = new DOMParser().parseFromString(
    `<!DOCTYPE html><base href="${BASE_HREF}">`, "text/html");
  const shallow = doc.cloneNode(false);
  assert_equals(shallow.baseURI, BASE_HREF, "before inserting <base>");

  const base = shallow.createElement("base");
  base.setAttribute("href", "http://example.org/other/");
  shallow.appendChild(base);
  assert_equals(shallow.baseURI, "http://example.org/other/",
    "an inserted <base> wins over the copied base URL");
}, "Inserting a <base> into a shallow clone overrides the copied base URL");

async_test(t => {
  const blobURL = URL.createObjectURL(
    new Blob(["<!doctype html>"], { type: "text/html" }));
  t.add_cleanup(() => URL.revokeObjectURL(blobURL));

  const iframe = document.createElement("iframe");
  iframe.src = blobURL;
  iframe.onload = t.step_func_done(() => {
    const doc = iframe.contentDocument;
    assert_equals(doc.baseURI, blobURL, "source");

    assert_equals(doc.cloneNode(true).baseURI, blobURL, "deep");
    assert_equals(doc.cloneNode(false).baseURI, blobURL, "shallow");
    iframe.remove();
  });
  document.body.appendChild(iframe);
}, "Clone of a blob: URL document");
