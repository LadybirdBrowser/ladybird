/**
 * Validations where `child` argument is irrelevant.
 * @param {Function} methodName
 */
function preInsertionValidateHierarchy(methodName) {
  function insert(parent, node) {
    if (parent[methodName].length > 1) {
      // This is for insertBefore(). We can't blindly pass `null` for all methods
      // as doing so will move nodes before validation.
      parent[methodName](node, null);
    } else {
      parent[methodName](node);
    }
  }

  // Step 2
  test(() => {
    const doc = document.implementation.createHTMLDocument("title");
    assert_throws_dom("HierarchyRequestError", () => insert(doc.body, doc.body));
    assert_throws_dom("HierarchyRequestError", () => insert(doc.body, doc.documentElement));
  }, "If node is a host-including inclusive ancestor of parent, then throw a HierarchyRequestError DOMException.");

  // Step 4
  test(() => {
    const doc = document.implementation.createHTMLDocument("title");
    const doc2 = document.implementation.createHTMLDocument("title2");
    assert_throws_dom("HierarchyRequestError", () => insert(doc, doc2));
  }, "If node is not a DocumentFragment, DocumentType, Element, Text, ProcessingInstruction, or Comment node, then throw a HierarchyRequestError DOMException.");

  // Step 5, in case of inserting a text node into a document
  test(() => {
    const doc = document.implementation.createHTMLDocument("title");
    assert_throws_dom("HierarchyRequestError", () => insert(doc, doc.createTextNode("text")));
  }, "If node is a Text node and parent is a document, then throw a HierarchyRequestError DOMException.");

  // Step 5, in case of inserting a doctype into a non-document
  test(() => {
    const doc = document.implementation.createHTMLDocument("title");
    const doctype = doc.childNodes[0];
    assert_throws_dom("HierarchyRequestError", () => insert(doc.createElement("a"), doctype));
  }, "If node is a doctype and parent is not a document, then throw a HierarchyRequestError DOMException.")

  // Step 6, in case of DocumentFragment including multiple elements
  test(() => {
    const doc = document.implementation.createHTMLDocument("title");
    doc.documentElement.remove();
    const df = doc.createDocumentFragment();
    df.appendChild(doc.createElement("a"));
    df.appendChild(doc.createElement("b"));
    assert_throws_dom("HierarchyRequestError", () => insert(doc, df));
  }, "If node is a DocumentFragment with multiple elements and parent is a document, then throw a HierarchyRequestError DOMException.");

  // Step 6, in case of DocumentFragment has multiple elements when document already has an element
  test(() => {
    const doc = document.implementation.createHTMLDocument("title");
    const df = doc.createDocumentFragment();
    df.appendChild(doc.createElement("a"));
    assert_throws_dom("HierarchyRequestError", () => insert(doc, df));
  }, "If node is a DocumentFragment with an element and parent is a document with another element, then throw a HierarchyRequestError DOMException.");

  // Step 6, in case of an element
  test(() => {
    const doc = document.implementation.createHTMLDocument("title");
    const el = doc.createElement("a");
    assert_throws_dom("HierarchyRequestError", () => insert(doc, el));
  }, "If node is an Element and parent is a document with another element, then throw a HierarchyRequestError DOMException.");

  // Step 6, in case of a doctype when document already has another doctype
  test(() => {
    const doc = document.implementation.createHTMLDocument("title");
    const doctype = doc.childNodes[0].cloneNode();
    doc.documentElement.remove();
    assert_throws_dom("HierarchyRequestError", () => insert(doc, doctype));
  }, "If node is a doctype and parent is a document with another doctype, then throw a HierarchyRequestError DOMException.");

  // Step 6, in case of a doctype when document has an element
  if (methodName !== "prepend") {
    // Skip `.prepend` as this doesn't throw if `child` is an element
    test(() => {
      const doc = document.implementation.createHTMLDocument("title");
      const doctype = doc.childNodes[0].cloneNode();
      doc.childNodes[0].remove();
      assert_throws_dom("HierarchyRequestError", () => insert(doc, doctype));
    }, "If node is a doctype and parent is a document with an element, then throw a HierarchyRequestError DOMException.");
  }
}
