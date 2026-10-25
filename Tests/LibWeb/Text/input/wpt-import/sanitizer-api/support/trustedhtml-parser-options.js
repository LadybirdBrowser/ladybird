const markup = "<div id=wrapper><span id=inner></span></div>";
const scriptMarkup = "<script>window.scriptRan = true;<" + "/script>";

const NO_OPTIONS = Symbol("no options");

const sinks = [
  ...["setHTMLUnsafe", "appendHTMLUnsafe", "prependHTMLUnsafe"].map(method => ({
    name: `Element.${method}`,
    sample: `Element ${method}`,
    hasRunScripts: true,
    createTarget(t) {
      const element = document.createElement("div");
      document.body.append(element);
      t.add_cleanup(() => element.remove());
      return element;
    },
    parse(target, html, options) {
      if (options === NO_OPTIONS) {
        target[method](html);
      } else {
        target[method](html, options);
      }
      return target;
    },
  })),
  ...["setHTMLUnsafe", "appendHTMLUnsafe", "prependHTMLUnsafe"].map(method => ({
    name: `ShadowRoot.${method}`,
    sample: `ShadowRoot ${method}`,
    hasRunScripts: true,
    createTarget(t) {
      const host = document.createElement("div");
      document.body.append(host);
      t.add_cleanup(() => host.remove());
      return host.attachShadow({ mode: "open" });
    },
    parse(target, html, options) {
      if (options === NO_OPTIONS) {
        target[method](html);
      } else {
        target[method](html, options);
      }
      return target;
    },
  })),
  ...["beforeHTMLUnsafe", "afterHTMLUnsafe", "replaceWithHTMLUnsafe"].map(method => ({
    name: `Node.${method}`,
    sample: `Node ${method}`,
    hasRunScripts: true,
    createTarget(t) {
      const parent = document.createElement("div");
      const target = document.createElement("div");
      parent.append(target);
      document.body.append(parent);
      t.add_cleanup(() => parent.remove());
      return target;
    },
    parse(target, html, options) {
      const parent = target.parentNode;
      if (options === NO_OPTIONS) {
        target[method](html);
      } else {
        target[method](html, options);
      }
      return parent;
    },
  })),
  {
    name: "Document.parseHTMLUnsafe",
    sample: "Document parseHTMLUnsafe",
    hasRunScripts: false,
    createTarget(t) {
      return null;
    },
    parse(target, html, options) {
      if (options === NO_OPTIONS) {
        return Document.parseHTMLUnsafe(html);
      }
      return Document.parseHTMLUnsafe(html, options);
    },
  },
];

// Options that need vetting by a default policy's createParserOptions.
const unvettedSanitizerOptions = [
  ["a sanitizer config", () => ({ sanitizer: { replaceWithChildrenElements: ["div"] } })],
  ["a sanitizer config without effect", () => ({ sanitizer: { removeElements: [] } })],
  ["a Sanitizer", () => ({ sanitizer: new Sanitizer({}) })],
  ['the "default" sanitizer preset', () => ({ sanitizer: "default" })],
  ["a sanitizer config and runScripts false", () => ({ sanitizer: { removeElements: ["span"] }, runScripts: false })],
];
const unvettedRunScriptsOptions = [
  ["runScripts true", () => ({ runScripts: true })],
  ["runScripts true and an empty sanitizer config", () => ({ sanitizer: {}, runScripts: true })],
];

// Options that don't need vetting.
const vettingFreeOptions = [
  ["no options", () => NO_OPTIONS],
  ["an empty dictionary", () => ({})],
  ["an empty sanitizer config", () => ({ sanitizer: {} })],
  ["runScripts false", () => ({ runScripts: false })],
];

function resetScriptRan(t) {
  window.scriptRan = false;
  t.add_cleanup(() => { delete window.scriptRan; });
}
