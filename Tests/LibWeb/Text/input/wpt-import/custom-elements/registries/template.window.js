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
  div.setHTMLUnsafe(`<div><template shadowrootmode=open shadowrootcustomelementregistry shadowrootserializable></template></div>`);
  assert_equals(div.firstChild.firstChild, null);
  assert_equals(div.getHTML({ serializableShadowRoots: true }), "<div><template shadowrootmode=\"open\" shadowrootserializable=\"\" shadowrootcustomelementregistry=\"\"></template></div>");
}, "Serializing a ShadowRoot with a null registry");

test(() => {
  const div = document.createElement("div");
  div.setHTMLUnsafe(`<div><template shadowrootmode=open shadowrootcustomelementregistry shadowrootserializable></template></div>`);
  const registry = new CustomElementRegistry();
  registry.initialize(div.firstChild.shadowRoot);
  assert_equals(div.firstChild.shadowRoot.customElementRegistry, registry);
  assert_equals(div.getHTML({ serializableShadowRoots: true }), "<div><template shadowrootmode=\"open\" shadowrootserializable=\"\" shadowrootcustomelementregistry=\"\"></template></div>");
}, "Serializing a ShadowRoot with a registry that differs from its host");
