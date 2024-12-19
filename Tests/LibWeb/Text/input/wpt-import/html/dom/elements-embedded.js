var embeddedElements = {
  picture: {},
  img: {
    // Conforming
    alt: "string",
    src: "url",
    srcset: "string",
    crossOrigin: {type: "enum", keywords: ["anonymous", "use-credentials"], nonCanon:{"": "anonymous"}, isNullable: true, defaultVal: null, invalidVal: "anonymous"},
    useMap: "string",
    isMap: "boolean",
    width: {type: "unsigned long", customGetter: true},
    height: {type: "unsigned long", customGetter: true},
    referrerPolicy: {type: "enum", keywords: ["", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin", "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url"]},
    decoding: {type: "enum", keywords: ["async", "sync", "auto"], defaultVal: "auto", invalidVal: "auto"},

    // Obsolete
    name: "string",
    lowsrc: {type: "url"},
    align: "string",
    hspace: "unsigned long",
    vspace: "unsigned long",
    longDesc: "url",
    border: {type: "string", treatNullAsEmptyString: true},
  },
  iframe: {
    // Conforming
    src: "url",
    srcdoc: "string",
    name: "string",
    sandbox: "settable tokenlist",
    allowFullscreen: "boolean",
    allowUserMedia: "boolean",
    width: "string",
    height: "string",
    referrerPolicy: {type: "enum", keywords: ["", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin", "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url"]},
    delegateStickyUserActivation: {type: "enum", keywords: ["vibration", "media"], defaultVal: null},

    // Obsolete
    align: "string",
    scrolling: "string",
    frameBorder: "string",
    longDesc: "url",
    marginHeight: {type: "string", treatNullAsEmptyString: true},
    marginWidth: {type: "string", treatNullAsEmptyString: true}
  },
  embed: {
    // Conforming
    src: "url",
    type: "string",
    width: "string",
    height: "string",

    // Obsolete
    align: "string",
    name: "string"
  },
  object: {
    // Conforming
    data: "url",
    type: "string",
    name: "string",
    useMap: "string",
    width: "string",
    height: "string",

    // Obsolete
    align: "string",
    archive: "string",
    code: "string",
    declare: "boolean",
    hspace: "unsigned long",
    standby: "string",
    vspace: "unsigned long",
    codeBase: "url",
    codeType: "string",
    border: {type: "string", treatNullAsEmptyString: true}
  },
  param: {
    // Conforming
    name: "string",
    value: "string",

    // Obsolete
    type: "string",
    valueType: "string"
  },
  video: {
    // HTMLMediaElement
    src: "url",
    crossOrigin: {type: "enum", keywords: ["anonymous", "use-credentials"], nonCanon:{"": "anonymous"}, isNullable: true, defaultVal: null, invalidVal: "anonymous"},
    // As with "keytype", we have no missing value default defined here.
    preload: {type: "enum", keywords: ["none", "metadata", "auto"], nonCanon: {"": "auto"}, defaultVal: null},
    autoplay: "boolean",
    loop: "boolean",
    controls: "boolean",
    controlsList: {type: "tokenlist", domAttrName: "controlsList"},
    defaultMuted: {type: "boolean", domAttrName: "muted"},

    width: "unsigned long",
    height: "unsigned long",
    poster: "url",
    playsInline: "boolean",
  },
  audio: {
    // HTMLMediaElement
    src: "url",
    crossOrigin: {type: "enum", keywords: ["anonymous", "use-credentials"], nonCanon:{"": "anonymous"}, isNullable: true, defaultVal: null, invalidVal: "anonymous"},
    // As with "keytype", we have no missing value default defined here.
    preload: {type: "enum", keywords: ["none", "metadata", "auto"], nonCanon: {"": "auto"}, defaultVal: null},
    autoplay: "boolean",
    loop: "boolean",
    controls: "boolean",
    defaultMuted: {type: "boolean", domAttrName: "muted"}
  },
  source: {
    src: "url",
    type: "string",
    srcset: "string",
    sizes: "string",
    media: "string"
  },
  track: {
    kind: {type: "enum", keywords: ["subtitles", "captions", "descriptions", "chapters", "metadata"], defaultVal: "subtitles", invalidVal: "metadata"},
    src: "url",
    srclang: "string",
    label: "string",
    "default": "boolean"
  },
  canvas: {
    width: {type: "unsigned long", defaultVal: 300},
    height: {type: "unsigned long", defaultVal: 150}
  },
  map: {
    name: "string"
  },
  area: {
    // Conforming
    alt: "string",
    coords: "string",
    shape: "string",
    target: "string",
    download: "string",
    ping: "string",
    rel: "string",
    relList: {type: "tokenlist", domAttrName: "rel"},
    referrerPolicy: {type: "enum", keywords: ["", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin", "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url"]},

    // HTMLHyperlinkElementUtils
    href: "url",

    // Obsolete
    noHref: "boolean"
  },
};

mergeElements(embeddedElements);
