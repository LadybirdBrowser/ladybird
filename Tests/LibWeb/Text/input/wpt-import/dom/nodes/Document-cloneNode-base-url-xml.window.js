// There is no xml:base: only an HTML <base> element (including one in the XHTML
// namespace) contributes to the document base URL.
// See https://github.com/whatwg/dom/issues/454

const BASE_HREF = "http://example.com/foo/bar";

test(() => {
  const doc = new DOMParser().parseFromString(
    `<root><base href="${BASE_HREF}"/></root>`, "application/xml");
  assert_equals(doc.baseURI, doc.URL, "source");

  assert_equals(doc.cloneNode(true).baseURI, doc.URL, "deep");
  assert_equals(doc.cloneNode(false).baseURI, doc.URL, "shallow");
}, "Clone of an XML document ignores a non-HTML <base> element");

test(() => {
  const doc = new DOMParser().parseFromString(
    `<html xmlns="http://www.w3.org/1999/xhtml">` +
    `<head><base href="${BASE_HREF}"/></head><body></body></html>`,
    "application/xhtml+xml");
  assert_equals(doc.baseURI, BASE_HREF, "source");

  assert_equals(doc.cloneNode(true).baseURI, BASE_HREF, "deep");
  assert_equals(doc.cloneNode(false).baseURI, BASE_HREF, "shallow");
}, "Clone of an XHTML document with a <base>");

test(() => {
  const doc = new DOMParser().parseFromString("<root/>", "application/xml");
  assert_equals(doc.baseURI, doc.URL, "source");

  assert_equals(doc.cloneNode(true).baseURI, doc.URL, "deep");
  assert_equals(doc.cloneNode(false).baseURI, doc.URL, "shallow");
}, "Clone of an XML document without a <base>");
