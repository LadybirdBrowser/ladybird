// Takes a root element and a list of ids of shadow host elements. Each id refers to a shadow host
// inside the previous id's shadow tree.
function getElementByShadowIds(root, ids) {
  for (var i = 0; ;i++) {
    var host = root.getElementById(ids[i]);
    if (host == null) {
      throw "No element found: i=" + i + " id=" + ids[i] + ". Root was " + root;
    }
    if (i == ids.length - 1) {
      return host;
    }
    root = host.shadowRoot;
    if (root == null) {
      throw "No shadowRoot found: i=" + i + " id=" + ids[i] + ". Host was " + host;
    }
  }
}

// Installs a mininal custom element based on this template.
function installCustomElement(element_name, template_id) {
  ceClass = class extends HTMLElement {
    constructor() {
      super();
      var template = document
        .getElementById(template_id)
        .content;
      this
        .attachShadow({mode: 'open'})
        .appendChild(template.cloneNode(true));
    }
  };
  window.customElements.define(element_name, ceClass);
}
