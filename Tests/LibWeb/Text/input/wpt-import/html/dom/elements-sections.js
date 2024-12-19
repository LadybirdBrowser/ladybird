var sectionElements = {
  body: {
    // Obsolete
    text: {type: "string", treatNullAsEmptyString: true},
    link: {type: "string", treatNullAsEmptyString: true},
    vLink: {type: "string", treatNullAsEmptyString: true},
    aLink: {type: "string", treatNullAsEmptyString: true},
    bgColor: {type: "string", treatNullAsEmptyString: true},
    background: "string",
  },
  article: {},
  section: {},
  nav: {},
  aside: {},
  h1: {
    // Obsolete
    align: "string",
  },
  h2: {
    // Obsolete
    align: "string",
  },
  h3: {
    // Obsolete
    align: "string",
  },
  h4: {
    // Obsolete
    align: "string",
  },
  h5: {
    // Obsolete
    align: "string",
  },
  h6: {
    // Obsolete
    align: "string",
  },
  hgroup: {},
  header: {},
  footer: {},
  address: {},
};

mergeElements(sectionElements);

extraTests.push(function() {
  ReflectionTests.reflects({type: "enum", keywords: ["ltr", "rtl", "auto"]}, "dir", document, "dir", document.documentElement);
  // TODO: these behave differently if the body element is a frameset.  Also
  // should probably test with multiple bodies.
  ReflectionTests.reflects({type: "string", treatNullAsEmptyString: true}, "fgColor", document, "text", document.body);
  ReflectionTests.reflects({type: "string", treatNullAsEmptyString: true}, "linkColor", document, "link", document.body);
  ReflectionTests.reflects({type: "string", treatNullAsEmptyString: true}, "vlinkColor", document, "vlink", document.body);
  ReflectionTests.reflects({type: "string", treatNullAsEmptyString: true}, "alinkColor", document, "alink", document.body);
  ReflectionTests.reflects({type: "string", treatNullAsEmptyString: true}, "bgColor", document, "bgcolor", document.body);
  // Edge remains RTL if we don't do this, despite removing the attribute
  document.dir = "ltr";
  // Don't mess up the colors :)
  document.documentElement.removeAttribute("dir");
  var attrs = ["text", "bgcolor", "link", "alink", "vlink"];
  for (var i = 0; i < attrs.length; i++) {
    document.body.removeAttribute(attrs[i]);
  }
});
