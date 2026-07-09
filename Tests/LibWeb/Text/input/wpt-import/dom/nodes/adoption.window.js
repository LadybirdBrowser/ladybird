// Testing DocumentFragment with host separately as it has a different node document by design
test(() => {
  const df = document.createElement("template").content;
  const child = df.appendChild(new Text('hi'));
  assert_not_equals(df.ownerDocument, document);
  const nodeDocument = df.ownerDocument;
  document.body.appendChild(df);
  assert_equals(df.childNodes.length, 0);
  assert_equals(child.ownerDocument, document);
  assert_equals(df.ownerDocument, nodeDocument);
}, `appendChild() and DocumentFragment with host`);

test(() => {
  const df = document.createElement("template").content;
  const child = df.appendChild(new Text('hi'));
  assert_not_equals(df.ownerDocument, document);
  assert_equals(document.adoptNode(df), df);
  assert_equals(df.childNodes.length, 1);
  assert_equals(child.ownerDocument, document);
  assert_equals(df.ownerDocument, document);
}, `adoptNode() and DocumentFragment with host`);

[
  {
    "name": "DocumentFragment",
    "creator": doc => doc.createDocumentFragment()
  },
  {
    "name": "ShadowRoot",
    "creator": doc => doc.createElementNS("http://www.w3.org/1999/xhtml", "div").attachShadow({mode: "closed"})
  }
].forEach(dfTest => {
  test(() => {
    const doc = new Document();
    const df = dfTest.creator(doc);
    const child = df.appendChild(new Text('hi'));
    assert_equals(df.ownerDocument, doc);

    document.body.appendChild(df);
    assert_equals(df.childNodes.length, 0);
    assert_equals(child.ownerDocument, document);
    assert_equals(df.ownerDocument, doc);
  }, `appendChild() and ${dfTest.name}`);

  test(() => {
    const doc = new Document();
    const df = dfTest.creator(doc);
    const child = df.appendChild(new Text('hi'));
    if (dfTest.name === "ShadowRoot") {
      assert_throws_dom("HierarchyRequestError", () => document.adoptNode(df));
    } else {
      document.adoptNode(df);
      assert_equals(df.childNodes.length, 1);
      assert_equals(child.ownerDocument, document);
      assert_equals(df.ownerDocument, document);
    }
  }, `adoptNode() and ${dfTest.name}`);
});
