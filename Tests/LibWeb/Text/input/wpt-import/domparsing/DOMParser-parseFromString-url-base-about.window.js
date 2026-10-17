// A document created by DOMParser gets the URL of the document it is created in, but not that
// document's inherited base URL.
// See https://github.com/whatwg/dom/issues/454

function loadIframe(t, configure) {
  return new Promise(resolve => {
    const iframe = document.createElement("iframe");
    configure(iframe);
    iframe.onload = () => resolve(iframe);
    t.add_cleanup(() => iframe.remove());
    document.body.appendChild(iframe);
  });
}

promise_test(async t => {
  const win = (await loadIframe(t, iframe => iframe.src = "about:blank")).contentWindow;
  const doc = new win.DOMParser().parseFromString("x", "text/html");
  assert_equals(doc.URL, "about:blank", "URL");
  assert_equals(doc.baseURI, "about:blank", "baseURI");
}, "DOMParser in an about:blank document");

promise_test(async t => {
  const win = (await loadIframe(t, iframe => iframe.srcdoc = "x")).contentWindow;
  const doc = new win.DOMParser().parseFromString("x", "text/html");
  assert_equals(doc.URL, "about:srcdoc", "URL");
  assert_equals(doc.baseURI, "about:srcdoc", "baseURI");
}, "DOMParser in an about:srcdoc document");
