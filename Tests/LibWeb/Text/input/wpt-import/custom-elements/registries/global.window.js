test(() => {
  const contentDocument = document.implementation.createHTMLDocument();
  assert_throws_dom("NotSupportedError", () => customElements.initialize(contentDocument));
  assert_throws_dom("NotSupportedError", () => customElements.initialize(contentDocument.createElement("x")));
}, "initialize() of global registry should throw for nodes from another document");

test(() => {
  const contentDocument = document.implementation.createHTMLDocument();
  assert_throws_dom("NotSupportedError", () => contentDocument.createElement("div", { customElementRegistry: customElements }));
}, "createElement() should throw with global registry from another document");

test(() => {
  const contentDocument = document.implementation.createHTMLDocument();
  assert_throws_dom("NotSupportedError", () => contentDocument.createElementNS("x", "div", { customElementRegistry: customElements }));
}, "createElementNS() should throw with global registry from another document");

test(() => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = contentDocument.createElement("div");
  assert_throws_dom("NotSupportedError", () => element.attachShadow({ mode: "closed", customElementRegistry: customElements }));
}, "attachShadow() should throw with global registry from another document");

test(() => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = contentDocument.createElement("div");
  assert_throws_dom("NotSupportedError", () => contentDocument.importNode(element, { customElementRegistry: customElements }));
}, "importNode() should throw with global registry from another document");
