test(() => {
  const template = document.createElement("template");
  assert_false(template.hasAttribute("shadowrootcustomelementregistry"));
  assert_equals(template.shadowRootCustomElementRegistry, "");

  template.shadowRootCustomElementRegistry = "blah";
  assert_equals(template.getAttribute("shadowrootcustomelementregistry"), "blah");
  assert_equals(template.shadowRootCustomElementRegistry, "blah");
}, "shadowRootCustomElementRegistry reflects as string");

test(() => {
  const div = document.createElement("div");
  div.setHTMLUnsafe(`<span><template shadowrootmode=open shadowrootcustomelementregistry shadowrootserializable></template></span>`);
  assert_equals(div.firstChild.firstChild, null);
  assert_equals(div.getHTML({ serializableShadowRoots: true }), "<span><template shadowrootmode=\"open\" shadowrootserializable=\"\" shadowrootcustomelementregistry=\"\"></template></span>");
}, "Serializing a null registry ShadowRoot with a global registry host (document)");

test(() => {
  const div = document.createElement("div");
  div.setHTMLUnsafe(`<span><template shadowrootmode=open shadowrootserializable></template></span>`);
  assert_equals(div.firstChild.firstChild, null);
  assert_equals(div.firstChild.shadowRoot.customElementRegistry, customElements);
  assert_equals(div.getHTML({ serializableShadowRoots: true }), "<span><template shadowrootmode=\"open\" shadowrootserializable=\"\"></template></span>");
}, "Serializing a global registry ShadowRoot with a global registry host (document)");

test(() => {
  const div = document.createElement("div");
  div.setHTMLUnsafe(`<span><template shadowrootmode=open shadowrootcustomelementregistry shadowrootserializable></template></span>`);
  const registry = new CustomElementRegistry();
  registry.initialize(div.firstChild.shadowRoot);
  assert_equals(div.firstChild.shadowRoot.customElementRegistry, registry);
  assert_equals(div.getHTML({ serializableShadowRoots: true }), "<span><template shadowrootmode=\"open\" shadowrootserializable=\"\" shadowrootcustomelementregistry=\"\"></template></span>");
}, "Serializing a scoped registry ShadowRoot with a global registry host (document)");

test(() => {
  const div = document.implementation.createHTMLDocument().createElement("div");
  assert_equals(div.customElementRegistry, null);
  div.setHTMLUnsafe(`<span><template shadowrootmode=open shadowrootcustomelementregistry shadowrootserializable></template></span>`);
  assert_equals(div.firstChild.shadowRoot.customElementRegistry, null);
  assert_equals(div.getHTML({ serializableShadowRoots: true }), "<span><template shadowrootmode=\"open\" shadowrootserializable=\"\"></template></span>");
}, "Serializing a null registry ShadowRoot with a null registry host (document)");

test(() => {
  const div = document.implementation.createHTMLDocument().createElement("div");
  assert_equals(div.customElementRegistry, null);
  div.setHTMLUnsafe(`<span><template shadowrootmode=open shadowrootcustomelementregistry shadowrootserializable></template></span>`);
  const registry = new CustomElementRegistry();
  registry.initialize(div.firstChild.shadowRoot);
  assert_equals(div.firstChild.shadowRoot.customElementRegistry, registry);
  assert_equals(div.getHTML({ serializableShadowRoots: true }), "<span><template shadowrootmode=\"open\" shadowrootserializable=\"\" shadowrootcustomelementregistry=\"\"></template></span>");
}, "Serializing a scoped registry ShadowRoot with a null registry host (document)");

test(() => {
  const registry = new CustomElementRegistry();
  const hostDocument = document.implementation.createHTMLDocument();
  registry.initialize(hostDocument);
  assert_equals(hostDocument.customElementRegistry, registry);
  const host = hostDocument.createElement('div');
  assert_equals(host.customElementRegistry, registry);
  const shadow = host.attachShadow({ mode: "closed", serializable: true, customElementRegistry: null });
  assert_equals(shadow.customElementRegistry, null);
  assert_equals(host.getHTML({ serializableShadowRoots: true }), `<template shadowrootmode="closed" shadowrootserializable="" shadowrootcustomelementregistry=""></template>`);
}, "Serializing a null registry ShadowRoot with a scoped registry host (document)");

test(() => {
  const registry = new CustomElementRegistry();
  const hostDocument = document.implementation.createHTMLDocument();
  registry.initialize(hostDocument);
  assert_equals(hostDocument.customElementRegistry, registry);
  const host = hostDocument.createElement('div');
  assert_equals(host.customElementRegistry, registry);
  const shadow = host.attachShadow({ mode: "closed", serializable: true, customElementRegistry: registry });
  assert_equals(shadow.customElementRegistry, registry);
  assert_equals(host.getHTML({ serializableShadowRoots: true }), `<template shadowrootmode="closed" shadowrootserializable="" shadowrootcustomelementregistry=""></template>`);
}, "Serializing a scoped registry ShadowRoot with a scoped registry host (document)");

test(() => {
  const registry = new CustomElementRegistry();
  const element = document.createElement('a-b', { customElementRegistry: registry });
  element.setHTMLUnsafe(`<a-b><template shadowrootmode="open"></template></a-b>`);
  assert_equals(element.firstChild.customElementRegistry, registry);
  assert_equals(element.firstChild.shadowRoot.customElementRegistry, customElements);
}, "A declarative shadow root gets its default registry from its node document");
