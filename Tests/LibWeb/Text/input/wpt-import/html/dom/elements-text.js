// Up-to-date as of 2013-04-19.
var textElements = {
  a: {
    // Conforming
    target: "string",
    download: "string",
    ping: "string",
    rel: "string",
    relList: {type: "tokenlist", domAttrName: "rel"},
    hreflang: "string",
    type: "string",
    referrerPolicy: {type: "enum", keywords: ["", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin", "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url"]},

    // HTMLHyperlinkElementUtils
    href: "url",

    // Obsolete
    coords: "string",
    charset: "string",
    name: "string",
    rev: "string",
    shape: "string",
  },
  em: {},
  strong: {},
  small: {},
  s: {},
  cite: {},
  q: {
    cite: "url",
  },
  dfn: {},
  abbr: {},
  ruby: {},
  rt: {},
  rp: {},
  data: {
    value: "string",
  },
  time: {
    dateTime: "string",
  },
  code: {},
  var: {},
  samp: {},
  kbd: {},
  sub: {},
  sup: {},
  i: {},
  b: {},
  u: {},
  mark: {},
  bdi: {},
  bdo: {},
  span: {},
  br: {
    // Obsolete
    clear: "string",
  },
  wbr: {},
};

mergeElements(textElements);
