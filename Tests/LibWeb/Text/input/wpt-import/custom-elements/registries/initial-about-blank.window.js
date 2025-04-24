async_test(t => {
  const frame = document.createElement("iframe");
  document.body.append(frame);
  frame.contentWindow.eval(`
    class AA extends HTMLElement { };
    self.globalAA = AA;
    customElements.define("a-a", AA);
    document.body.innerHTML = "<a-a>";
  `);
  assert_equals(frame.contentDocument.body.firstChild.localName, "a-a");
  assert_true(frame.contentDocument.body.firstChild instanceof frame.contentWindow.globalAA);

  const blankDocumentURL = new URL("/common/blank.html", location).href;
  frame.src = blankDocumentURL;
  frame.onload = t.step_func_done(t => {
    assert_equals(frame.contentDocument.URL, blankDocumentURL);
    assert_equals(frame.contentDocument.body.innerHTML, "");
    frame.contentDocument.body.innerHTML = "<a-a>";
    assert_equals(frame.contentDocument.body.firstChild.localName, "a-a");
    assert_equals(frame.contentWindow.customElements.get("a-a"), undefined);
    assert_not_equals(frame.contentWindow.globalAA, undefined);
  });
}, "Each navigable document has its own registry");
