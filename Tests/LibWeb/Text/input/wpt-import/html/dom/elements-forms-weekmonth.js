var formElements = {
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
    type: {type: "enum", keywords: ["month", "week"],
      defaultVal: "text"},
    width: {type: "unsigned long", customGetter: true},
    defaultValue: {type: "string", domAttrName: "value"},

    // Obsolete
    align: "string",
    useMap: "string",
  },
};

mergeElements(formElements);
