// Target document has a global registry

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const element = document.createElement('div');
  assert_equals(element.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  assert_equals(element.customElementRegistry, contentDocument.customElementRegistry);
}, "Adoption with global registry");

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const element = document.createElement('div', { customElementRegistry: customElements });
  assert_equals(element.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  assert_equals(element.customElementRegistry, contentDocument.customElementRegistry);
}, "Adoption with explicit global registry");

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const scoped = new CustomElementRegistry();
  const element = document.createElement('div', { customElementRegistry: scoped });
  assert_equals(element.customElementRegistry, scoped);

  contentDocument.body.appendChild(element);
  assert_equals(element.customElementRegistry, scoped);
}, "Adoption with scoped registry");

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const element = document.createElement('div');
  assert_equals(element.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(element.customElementRegistry, contentDocument.customElementRegistry);
}, "Adoption with global registry into a scoped registry");

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const element = document.createElement('div', { customElementRegistry: customElements });
  assert_equals(element.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(element.customElementRegistry, contentDocument.customElementRegistry);
}, "Adoption with explicit global registry into a scoped registry");

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const scoped = new CustomElementRegistry();
  const element = document.createElement('div', { customElementRegistry: scoped });
  assert_equals(element.customElementRegistry, scoped);

  const scoped2 = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped2 });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped2);
  scopedElement.appendChild(element);
  assert_equals(element.customElementRegistry, scoped);
}, "Adoption with scoped registry into a scoped registry");

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed" });
  assert_equals(elementShadow.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  // In certain implementations touching element.customElementRegistry can poison the results so we
  // don't do that here.
  assert_equals(elementShadow.customElementRegistry, contentDocument.customElementRegistry);
}, "Adoption including shadow root with global registry");

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: customElements });
  assert_equals(elementShadow.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, contentDocument.customElementRegistry);
}, "Adoption including shadow root with explicit global registry");

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const element = document.createElement('div');
  const scoped = new CustomElementRegistry();
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: scoped });
  assert_equals(elementShadow.customElementRegistry, scoped);

  contentDocument.body.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, scoped);
}, "Adoption including shadow root with scoped registry");

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed" });
  assert_equals(elementShadow.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, contentDocument.customElementRegistry);
}, "Adoption including shadow root with global registry into a scoped registry");

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: customElements });
  assert_equals(elementShadow.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, contentDocument.customElementRegistry);
}, "Adoption including shadow root with explicit global registry into a scoped registry");

test(t => {
  const contentDocument = document.body.appendChild(document.createElement('iframe')).contentDocument;
  t.add_cleanup(() => contentDocument.defaultView.frameElement.remove());
  const element = document.createElement('div');
  const scoped = new CustomElementRegistry();
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: scoped });
  assert_equals(elementShadow.customElementRegistry, scoped);

  const scoped2 = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped2 });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped2);
  scopedElement.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, scoped);
}, "Adoption including shadow root with scoped registry into a scoped registry");


// Target document has a null registry

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = document.createElement('div');
  assert_equals(element.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  assert_equals(element.customElementRegistry, null);
}, "Adoption with global registry (null registry target)");

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = document.createElement('div', { customElementRegistry: customElements });
  assert_equals(element.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  assert_equals(element.customElementRegistry, null);
}, "Adoption with explicit global registry (null registry target)");

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const scoped = new CustomElementRegistry();
  const element = document.createElement('div', { customElementRegistry: scoped });
  assert_equals(element.customElementRegistry, scoped);

  contentDocument.body.appendChild(element);
  assert_equals(element.customElementRegistry, scoped);
}, "Adoption with scoped registry (null registry target)");

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = document.createElement('div');
  assert_equals(element.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(element.customElementRegistry, null);
}, "Adoption with global registry into a scoped registry (null registry target)");

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = document.createElement('div', { customElementRegistry: customElements });
  assert_equals(element.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(element.customElementRegistry, null);
}, "Adoption with explicit global registry into a scoped registry (null registry target)");

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const scoped = new CustomElementRegistry();
  const element = document.createElement('div', { customElementRegistry: scoped });
  assert_equals(element.customElementRegistry, scoped);

  const scoped2 = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped2 });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped2);
  scopedElement.appendChild(element);
  assert_equals(element.customElementRegistry, scoped);
}, "Adoption with scoped registry into a scoped registry (null registry target)");

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed" });
  assert_equals(elementShadow.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  // In certain implementations touching element.customElementRegistry can poison the results so we
  // don't do that here.
  assert_equals(elementShadow.customElementRegistry, null);
}, "Adoption including shadow root with global registry (null registry target)");

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: customElements });
  assert_equals(elementShadow.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, null);
}, "Adoption including shadow root with explicit global registry (null registry target)");

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = document.createElement('div');
  const scoped = new CustomElementRegistry();
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: scoped });
  assert_equals(elementShadow.customElementRegistry, scoped);

  contentDocument.body.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, scoped);
}, "Adoption including shadow root with scoped registry (null registry target)");

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed" });
  assert_equals(elementShadow.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, null);
}, "Adoption including shadow root with global registry into a scoped registry (null registry target)");

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: customElements });
  assert_equals(elementShadow.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, null);
}, "Adoption including shadow root with explicit global registry into a scoped registry (null registry target)");

test(t => {
  const contentDocument = document.implementation.createHTMLDocument();
  const element = document.createElement('div');
  const scoped = new CustomElementRegistry();
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: scoped });
  assert_equals(elementShadow.customElementRegistry, scoped);

  const scoped2 = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped2 });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped2);
  scopedElement.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, scoped);
}, "Adoption including shadow root with scoped registry into a scoped registry (null registry target)");


// Target document has a scoped registry

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);
  const element = document.createElement('div');
  assert_equals(element.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  assert_equals(element.customElementRegistry, null);
}, "Adoption with global registry (scoped registry target)");

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);

  const element = document.createElement('div', { customElementRegistry: customElements });
  assert_equals(element.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  assert_equals(element.customElementRegistry, null);
}, "Adoption with explicit global registry (scoped registry target)");

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);

  const scoped = new CustomElementRegistry();
  const element = document.createElement('div', { customElementRegistry: scoped });
  assert_equals(element.customElementRegistry, scoped);

  contentDocument.body.appendChild(element);
  assert_equals(element.customElementRegistry, scoped);
}, "Adoption with scoped registry (scoped registry target)");

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);

  const element = document.createElement('div');
  assert_equals(element.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(element.customElementRegistry, null);
}, "Adoption with global registry into a scoped registry (scoped registry target)");

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);

  const element = document.createElement('div', { customElementRegistry: customElements });
  assert_equals(element.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(element.customElementRegistry, null);
}, "Adoption with explicit global registry into a scoped registry (scoped registry target)");

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);

  const scoped = new CustomElementRegistry();
  const element = document.createElement('div', { customElementRegistry: scoped });
  assert_equals(element.customElementRegistry, scoped);

  const scoped2 = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped2 });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped2);
  scopedElement.appendChild(element);
  assert_equals(element.customElementRegistry, scoped);
}, "Adoption with scoped registry into a scoped registry (scoped registry target)");

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);

  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed" });
  assert_equals(elementShadow.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  // In certain implementations touching element.customElementRegistry can poison the results so we
  // don't do that here.
  assert_equals(elementShadow.customElementRegistry, null);
}, "Adoption including shadow root with global registry (scoped registry target)");

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);

  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: customElements });
  assert_equals(elementShadow.customElementRegistry, customElements);

  contentDocument.body.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, null);
}, "Adoption including shadow root with explicit global registry (scoped registry target)");

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);

  const element = document.createElement('div');
  const scoped = new CustomElementRegistry();
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: scoped });
  assert_equals(elementShadow.customElementRegistry, scoped);

  contentDocument.body.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, scoped);
}, "Adoption including shadow root with scoped registry (scoped registry target)");

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);

  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed" });
  assert_equals(elementShadow.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, null);
}, "Adoption including shadow root with global registry into a scoped registry (scoped registry target)");

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);
  const element = document.createElement('div');
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: customElements });
  assert_equals(elementShadow.customElementRegistry, customElements);

  const scoped = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped);
  scopedElement.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, null);
}, "Adoption including shadow root with explicit global registry into a scoped registry (scoped registry target)");

test(t => {
  const documentRegistry = new CustomElementRegistry();
  const contentDocument = document.implementation.createHTMLDocument();
  documentRegistry.initialize(contentDocument);
  assert_equals(contentDocument.customElementRegistry, documentRegistry);
  const element = document.createElement('div');
  const scoped = new CustomElementRegistry();
  const elementShadow = element.attachShadow({ mode: "closed", customElementRegistry: scoped });
  assert_equals(elementShadow.customElementRegistry, scoped);

  const scoped2 = new CustomElementRegistry();
  const scopedElement = contentDocument.createElement('div', { customElementRegistry: scoped2 });
  contentDocument.body.appendChild(scopedElement);
  assert_equals(scopedElement.customElementRegistry, scoped2);
  scopedElement.appendChild(element);
  assert_equals(elementShadow.customElementRegistry, scoped);
}, "Adoption including shadow root with scoped registry into a scoped registry (scoped registry target)");
