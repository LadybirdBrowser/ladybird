test(() => {
  const otherDocument = new Document();
  const element = otherDocument.createElement("blah");
  assert_true(element.matches(":defined"));
  const registry = new CustomElementRegistry();
  registry.initialize(element);
  assert_equals(element.customElementRegistry, registry);
  assert_true(element.matches(":defined"));
}, `"uncustomized" :defined doesn't care about your registry'`);

test(() => {
  const registry = new CustomElementRegistry();
  registry.define("sw-r2d2", class extends HTMLElement {});
  const element = document.createElement("sw-r2d2", { customElementRegistry: registry });
  assert_equals(element.customElementRegistry, registry);
  assert_true(element.matches(":defined"));
}, `"custom" :defined doesn't care about your registry`);

test(() => {
  const otherDocument = new Document();
  const element = otherDocument.createElementNS("http://www.w3.org/1999/xhtml", "sw-r2d2");
  assert_false(element.matches(":defined"));
  const registry = new CustomElementRegistry();
  registry.define("sw-r2d2", class extends HTMLElement {});
  registry.initialize(element);
  assert_true(element.matches(":defined"));
}, `"custom" :defined should apply after initialize`);
