var metadataElements = {
  head: {},
  title: {},
  base: {
    href: {type: "url", customGetter: true},
    target: "string",
  },
  link: {
    // Conforming
    href: "url",
    crossOrigin: {type: "enum", keywords: ["anonymous", "use-credentials"], nonCanon:{"": "anonymous"}, isNullable: true, defaultVal: null, invalidVal: "anonymous"},
    rel: "string",
    as: {
      type: "enum",
      keywords: ["fetch", "audio", "document", "embed", "font", "image", "manifest", "object", "report", "script", "sharedworker", "style", "track", "video", "worker", "xslt"],
      defaultVal: "",
      invalidVal: ""
    },
    relList: {type: "tokenlist", domAttrName: "rel"},
    media: "string",
    nonce: "string",
    integrity: "string",
    hreflang: "string",
    type: "string",
    sizes: "settable tokenlist",
    referrerPolicy: {type: "enum", keywords: ["", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin", "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url"]},

    // Obsolete
    charset: "string",
    rev: "string",
    target: "string",
  },
  meta: {
    // Conforming
    name: "string",
    httpEquiv: {type: "string", domAttrName: "http-equiv"},
    content: "string",
    media: "string",

    // Obsolete
    scheme: "string",
  },
  style: {
    media: "string",
    nonce: "string",
    type: "string",
  },
};

mergeElements(metadataElements);
