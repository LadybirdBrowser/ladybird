// Up-to-date as of 2013-04-12.
var tabularElements = {
  table: {
    // Obsolete
    align: "string",
    border: "string",
    frame: "string",
    rules: "string",
    summary: "string",
    width: "string",
    bgColor: {type: "string", treatNullAsEmptyString: true},
    cellPadding: {type: "string", treatNullAsEmptyString: true},
    cellSpacing: {type: "string", treatNullAsEmptyString: true},
  },
  caption: {
    // Obsolete
    align: "string",
  },
  colgroup: {
    span: {type: "clamped unsigned long", defaultVal: 1, min: 1, max: 1000},

    // Obsolete
    align: "string",
    ch: {type: "string", domAttrName: "char"},
    chOff: {type: "string", domAttrName: "charoff"},
    vAlign: "string",
    width: "string",
  },
  col: {
    // Conforming
    span: {type: "clamped unsigned long", defaultVal: 1, min: 1, max: 1000},

    // Obsolete
    align: "string",
    ch: {type: "string", domAttrName: "char"},
    chOff: {type: "string", domAttrName: "charoff"},
    vAlign: "string",
    width: "string",
  },
  tbody: {
    // Obsolete
    align: "string",
    ch: {type: "string", domAttrName: "char"},
    chOff: {type: "string", domAttrName: "charoff"},
    vAlign: "string",
  },
  thead: {
    // Obsolete
    align: "string",
    ch: {type: "string", domAttrName: "char"},
    chOff: {type: "string", domAttrName: "charoff"},
    vAlign: "string",
  },
  tfoot: {
    // Obsolete
    align: "string",
    ch: {type: "string", domAttrName: "char"},
    chOff: {type: "string", domAttrName: "charoff"},
    vAlign: "string",
  },
  tr: {
    // Obsolete
    align: "string",
    ch: {type: "string", domAttrName: "char"},
    chOff: {type: "string", domAttrName: "charoff"},
    vAlign: "string",
    bgColor: {type: "string", treatNullAsEmptyString: true},
  },
  td: {
    // HTMLTableCellElement (Conforming)
    colSpan: {type: "clamped unsigned long", defaultVal: 1, min: 1, max: 1000},
    rowSpan: {type: "clamped unsigned long", defaultVal: 1, min: 0, max: 65534},
    headers: "string",
    scope: {type: "enum", keywords: ["row", "col", "rowgroup", "colgroup"]},
    abbr: "string",

    // HTMLTableCellElement (Obsolete)
    align: "string",
    axis: "string",
    height: "string",
    width: "string",
    ch: {type: "string", domAttrName: "char"},
    chOff: {type: "string", domAttrName: "charoff"},
    noWrap: "boolean",
    vAlign: "string",
    bgColor: {type: "string", treatNullAsEmptyString: true},
  },
  th: {
    // HTMLTableCellElement (Conforming)
    colSpan: {type: "clamped unsigned long", defaultVal: 1, min: 1, max: 1000},
    rowSpan: {type: "clamped unsigned long", defaultVal: 1, min: 0, max: 65534},
    headers: "string",
    scope: {type: "enum", keywords: ["row", "col", "rowgroup", "colgroup"]},
    abbr: "string",

    // HTMLTableCellElement (Obsolete)
    align: "string",
    axis: "string",
    height: "string",
    width: "string",
    ch: {type: "string", domAttrName: "char"},
    chOff: {type: "string", domAttrName: "charoff"},
    noWrap: "boolean",
    vAlign: "string",
    bgColor: {type: "string", treatNullAsEmptyString: true},
  },
};

mergeElements(tabularElements);
