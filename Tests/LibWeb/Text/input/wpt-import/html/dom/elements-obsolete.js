var obsoleteElements = {
  marquee: {
    behavior: {
      type: {
        type: "enum",
        keywords: ["scroll", "slide", "alternate"],
        defaultVal: "scroll"
      },
    },
    bgColor: "string",
    direction: {
      type: {
        type: "enum",
        keywords: ["up", "right", "down", "left"],
        defaultVal: "left"
      },
    },
    height: "string",
    hspace: "unsigned long",
    scrollAmount: {type: "unsigned long", defaultVal: 6},
    scrollDelay: {type: "unsigned long", defaultVal: 85},
    trueSpeed: "boolean",
    vspace: "unsigned long",
    width: "string",
  },
  frameset: {
    cols: "string",
    rows: "string",
  },
  frame: {
    name: "string",
    scrolling: "string",
    src: "url",
    frameBorder: "string",
    longDesc: "url",
    noResize: "boolean",
    marginHeight: {type: "string", treatNullAsEmptyString: true},
    marginWidth: {type: "string", treatNullAsEmptyString: true},
  },
  dir: {
    compact: "boolean",
  },
  font: {
    color: {type: "string", treatNullAsEmptyString: true},
    face: "string",
    size: "string",
  },
};

mergeElements(obsoleteElements);
