var formElements = {
  form: {
    acceptCharset: {type: "string", domAttrName: "accept-charset"},
    // "action" has magic hard-coded in reflection.js
    action: "url",
    autocomplete: {type: "enum", keywords: ["on", "off"], defaultVal: "on"},
    enctype: {type: "enum", keywords: ["application/x-www-form-urlencoded", "multipart/form-data", "text/plain"], defaultVal: "application/x-www-form-urlencoded"},
    encoding: {type: "enum", keywords: ["application/x-www-form-urlencoded", "multipart/form-data", "text/plain"], defaultVal: "application/x-www-form-urlencoded", domAttrName: "enctype"},
    method: {type: "enum", keywords: ["get", "post", "dialog"], defaultVal: "get"},
    name: "string",
    noValidate: "boolean",
    target: "string",
  },
  fieldset: {
    disabled: "boolean",
    name: "string",
  },
  legend: {
    // Obsolete
    align: "string",
  },
  label: {
    htmlFor: {type: "string", domAttrName: "for"},
  },
  input: {
    // Conforming
    accept: "string",
    alt: "string",
    autocomplete: {type: "string", customGetter: true},
    defaultChecked: {type: "boolean", domAttrName: "checked"},
    dirName: "string",
    disabled: "boolean",
    // "formAction" has magic hard-coded in reflection.js
    formAction: "url",
    formEnctype: {type: "enum", keywords: ["application/x-www-form-urlencoded", "multipart/form-data", "text/plain"], invalidVal: "application/x-www-form-urlencoded"},
    formMethod: {type: "enum", keywords: ["get", "post"], invalidVal: "get"},
    formNoValidate: "boolean",
    formTarget: "string",
    height: {type: "unsigned long", customGetter: true},
    max: "string",
    maxLength: "limited long",
    min: "string",
    minLength: "limited long",
    multiple: "boolean",
    name: "string",
    pattern: "string",
    placeholder: "string",
    readOnly: "boolean",
    required: "boolean",
    // https://html.spec.whatwg.org/#attr-input-size
    size: {type: "limited unsigned long", defaultVal: 20},
    src: "url",
    step: "string",
    type: {type: "enum", keywords: ["hidden", "text", "search", "tel",
      "url", "email", "password", "date",
      "time", "datetime-local", "number", "range", "color", "checkbox",
      "radio", "file", "submit", "image", "reset", "button"], defaultVal:
      "text"},
    width: {type: "unsigned long", customGetter: true},
    defaultValue: {type: "string", domAttrName: "value"},

    // Obsolete
    align: "string",
    useMap: "string",
  },
  button: {
    disabled: "boolean",
    // "formAction" has magic hard-coded in reflection.js
    formAction: "url",
    formEnctype: {type: "enum", keywords: ["application/x-www-form-urlencoded", "multipart/form-data", "text/plain"], invalidVal: "application/x-www-form-urlencoded"},
    formMethod: {type: "enum", keywords: ["get", "post", "dialog"], invalidVal: "get"},
    formNoValidate: "boolean",
    formTarget: "string",
    name: "string",
    type: {type: "enum", keywords: ["submit", "reset", "button"], defaultVal: "submit"},
    value: "string"
  },
  select: {
    autocomplete: {type: "string", customGetter: true},
    disabled: "boolean",
    multiple: "boolean",
    name: "string",
    required: "boolean",
    size: {type: "unsigned long", defaultVal: 0},
  },
  datalist: {},
  optgroup: {
    disabled: "boolean",
    label: "string",
  },
  option: {
    disabled: "boolean",
    label: {type: "string", customGetter: true},
    defaultSelected: {type: "boolean", domAttrName: "selected"},
    value: {type: "string", customGetter: true},
  },
  textarea: {
    autocomplete: {type: "string", customGetter: true},
    cols: {type: "limited unsigned long with fallback", defaultVal: 20},
    dirName: "string",
    disabled: "boolean",
    maxLength: "limited long",
    minLength: "limited long",
    name: "string",
    placeholder: "string",
    readOnly: "boolean",
    required: "boolean",
    rows: {type: "limited unsigned long with fallback", defaultVal: 2},
    wrap: "string",
  },
  output: {
    htmlFor: {type: "settable tokenlist", domAttrName: "for" },
    name: "string",
  },
  progress: {
    max: {type: "limited double", defaultVal: 1.0},
  },
  meter: {
    value: {type: "double", customGetter: true},
    min: {type: "double", customGetter: true},
    max: {type: "double", customGetter: true},
    low: {type: "double", customGetter: true},
    high: {type: "double", customGetter: true},
    optimum: {type: "double", customGetter: true},
  },
};

mergeElements(formElements);
