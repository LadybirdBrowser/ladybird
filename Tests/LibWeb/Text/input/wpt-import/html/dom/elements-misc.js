var miscElements = {
  // "The root element" section
  html: {
    // Obsolete
    version: "string",
  },

  // "Scripting" section
  script: {
    src: "url",
    type: "string",
    noModule: "boolean",
    charset: "string",
    // TODO: async attribute (complicated).
    defer: "boolean",
    crossOrigin: {type: "enum", keywords: ["anonymous", "use-credentials"], nonCanon:{"": "anonymous"}, isNullable: true, defaultVal: null, invalidVal: "anonymous"},
    integrity: "string",

    // Obsolete
    event: "string",
    htmlFor: {type: "string", domAttrName: "for"},
  },
  noscript: {},

  template: {},
  slot: {
    name: "string",
  },

  // "Edits" section
  ins: {
    cite: "url",
    dateTime: "string",
  },
  del: {
    cite: "url",
    dateTime: "string",
  },

  // "Interactive elements" section
  details: {
    open: "boolean",
  },
  summary: {},
  menu: {
    // Obsolete
    compact: "boolean",
  },
  dialog: {
    open: "boolean",
  },

  // Global attributes should exist even on unknown elements
  undefinedelement: {
    enterKeyHint: {type: "enum", keywords: ["enter", "done", "go", "next", "previous", "search", "send"]},
    inputMode: {type: "enum", keywords: ["none", "text", "tel", "url", "email", "numeric", "decimal", "search"]},
  },
};

mergeElements(miscElements);
