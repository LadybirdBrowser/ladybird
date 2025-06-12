test(t => {
  const frame = document.body.appendChild(document.createElement("iframe"));
  t.add_cleanup(() => frame.contentDocument.close());
  assert_equals(frame.contentDocument.compatMode, "BackCompat");
  frame.contentDocument.open();
  assert_equals(frame.contentDocument.compatMode, "CSS1Compat");
  frame.contentDocument.close();
  assert_equals(frame.contentDocument.compatMode, "BackCompat");
}, "document.open() sets document to no-quirks mode (write no doctype)");

test(t => {
  const frame = document.body.appendChild(document.createElement("iframe"));
  t.add_cleanup(() => frame.contentDocument.close());
  assert_equals(frame.contentDocument.compatMode, "BackCompat");
  frame.contentDocument.open();
  assert_equals(frame.contentDocument.compatMode, "CSS1Compat");
  frame.contentDocument.write("<!doctype html public");
  assert_equals(frame.contentDocument.compatMode, "CSS1Compat");
  frame.contentDocument.write(" \"-//IETF//DTD HTML 3//\"");
  assert_equals(frame.contentDocument.compatMode, "CSS1Compat");
  frame.contentDocument.write(">");
  assert_equals(frame.contentDocument.compatMode, "BackCompat");
  frame.contentDocument.close();
  assert_equals(frame.contentDocument.compatMode, "BackCompat");
}, "document.open() sets document to no-quirks mode (write old doctype)");

test(t => {
  const frame = document.body.appendChild(document.createElement("iframe"));
  t.add_cleanup(() => frame.contentDocument.close());
  assert_equals(frame.contentDocument.compatMode, "BackCompat");
  frame.contentDocument.open();
  assert_equals(frame.contentDocument.compatMode, "CSS1Compat");
  frame.contentDocument.write("<!doctype html");
  assert_equals(frame.contentDocument.compatMode, "CSS1Compat");
  frame.contentDocument.write(">");
  assert_equals(frame.contentDocument.compatMode, "CSS1Compat");
  frame.contentDocument.close();
  assert_equals(frame.contentDocument.compatMode, "CSS1Compat");
}, "document.open() sets document to no-quirks mode (write new doctype)");

// This tests the document.open() call in fact sets the document to no-quirks
// mode, not limited-quirks mode. It is derived from
// quirks/blocks-ignore-line-height.html in WPT, as there is no direct way to
// distinguish between a no-quirks document and a limited-quirks document. It
// assumes that the user agent passes the linked test, which at the time of
// writing is all major web browsers.
test(t => {
  const frame = document.body.appendChild(document.createElement("iframe"));
  t.add_cleanup(() => frame.contentDocument.close());
  assert_equals(frame.contentDocument.compatMode, "BackCompat");
  frame.contentDocument.open();
  assert_equals(frame.contentDocument.compatMode, "CSS1Compat");

  // Create the DOM tree manually rather than going through document.write() to
  // bypass the parser, which resets the document mode.
  const html = frame.contentDocument.appendChild(frame.contentDocument.createElement("html"));
  const body = html.appendChild(frame.contentDocument.createElement("body"));
  assert_equals(frame.contentDocument.body, body);
  body.innerHTML = `
    <style>#ref { display:block }</style>
    <div id=test><font size=1>x</font></div>
    <font id=ref size=1>x</font>
    <div id=s_ref>x</div>
  `;
  assert_equals(frame.contentDocument.compatMode, "CSS1Compat");

  const idTest = frame.contentDocument.getElementById("test");
  const idRef = frame.contentDocument.getElementById("ref");
  const idSRef = frame.contentDocument.getElementById("s_ref");
  assert_equals(frame.contentWindow.getComputedStyle(idTest).height,
                frame.contentWindow.getComputedStyle(idSRef).height);
  assert_not_equals(frame.contentWindow.getComputedStyle(idTest).height,
                    frame.contentWindow.getComputedStyle(idRef).height);
}, "document.open() sets document to no-quirks mode, not limited-quirks mode");
