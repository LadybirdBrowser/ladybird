// META: script=enctypes-helper.js

// Form submissions in multipart/form-data are also tested in
// /FileAPI/file/send-file*

// The `expected` property of objects passed to `formTest` must be an object
// with `name`, `value` and optionally `filename` properties, which represent
// the corresponding data in a multipart/form-data part.
const formTest = formSubmissionTemplate(
  "multipart/form-data",
  ({ name, filename, value }, serialized) => {
    let headers;
    if (filename === undefined) {
      headers = [`Content-Disposition: form-data; name="${name}"`];
    } else {
      headers = [
        `Content-Disposition: form-data; name="${name}"; filename="${filename}"`,
        "Content-Type: text/plain",
      ];
    }

    const boundary = serialized.split("\r\n")[0];

    return [
      boundary,
      ...headers,
      "",
      value,
      boundary + "--",
      "",
    ].join("\r\n");
  },
);

formTest({
  name: "basic",
  value: "test",
  expected: {
    name: "basic",
    value: "test",
  },
  description: "Basic test",
});

formTest({
  name: "basic",
  value: new File([], "file-test.txt", { type: "text/plain" }),
  expected: {
    name: "basic",
    filename: "file-test.txt",
    value: "",
  },
  description: "Basic File test",
});

formTest({
  name: "a\0b",
  value: "c",
  expected: {
    name: "a\0b",
    value: "c",
  },
  description: "0x00 in name",
});

formTest({
  name: "a",
  value: "b\0c",
  expected: {
    name: "a",
    value: "b\0c",
  },
  description: "0x00 in value",
});

formTest({
  name: "a",
  value: new File([], "b\0c", { type: "text/plain" }),
  expected: {
    name: "a",
    filename: "b\0c",
    value: "",
  },
  description: "0x00 in filename",
});

formTest({
  name: "a\nb",
  value: "c",
  expected: {
    name: "a%0D%0Ab",
    value: "c",
  },
  description: "\\n in name",
});

formTest({
  name: "a\rb",
  value: "c",
  expected: {
    name: "a%0D%0Ab",
    value: "c",
  },
  description: "\\r in name",
});

formTest({
  name: "a\r\nb",
  value: "c",
  expected: {
    name: "a%0D%0Ab",
    value: "c",
  },
  description: "\\r\\n in name",
});

formTest({
  name: "a\n\rb",
  value: "c",
  expected: {
    name: "a%0D%0A%0D%0Ab",
    value: "c",
  },
  description: "\\n\\r in name",
});

formTest({
  name: "a",
  value: "b\nc",
  expected: {
    name: "a",
    value: "b\r\nc",
  },
  description: "\\n in value",
});

formTest({
  name: "a",
  value: "b\rc",
  expected: {
    name: "a",
    value: "b\r\nc",
  },
  description: "\\r in value",
});

formTest({
  name: "a",
  value: "b\r\nc",
  expected: {
    name: "a",
    value: "b\r\nc",
  },
  description: "\\r\\n in value",
});

formTest({
  name: "a",
  value: "b\n\rc",
  expected: {
    name: "a",
    value: "b\r\n\r\nc",
  },
  description: "\\n\\r in value",
});

formTest({
  name: "a",
  value: new File([], "b\nc", { type: "text/plain" }),
  expected: {
    name: "a",
    filename: "b%0Ac",
    value: "",
  },
  description: "\\n in filename",
});

formTest({
  name: "a",
  value: new File([], "b\rc", { type: "text/plain" }),
  expected: {
    name: "a",
    filename: "b%0Dc",
    value: "",
  },
  description: "\\r in filename",
});

formTest({
  name: "a",
  value: new File([], "b\r\nc", { type: "text/plain" }),
  expected: {
    name: "a",
    filename: "b%0D%0Ac",
    value: "",
  },
  description: "\\r\\n in filename",
});

formTest({
  name: "a",
  value: new File([], "b\n\rc", { type: "text/plain" }),
  expected: {
    name: "a",
    filename: "b%0A%0Dc",
    value: "",
  },
  description: "\\n\\r in filename",
});

formTest({
  name: 'a"b',
  value: "c",
  expected: {
    name: "a%22b",
    value: "c",
  },
  description: "double quote in name",
});

formTest({
  name: "a",
  value: 'b"c',
  expected: {
    name: "a",
    value: 'b"c',
  },
  description: "double quote in value",
});

formTest({
  name: "a",
  value: new File([], 'b"c', { type: "text/plain" }),
  expected: {
    name: "a",
    filename: "b%22c",
    value: "",
  },
  description: "double quote in filename",
});

formTest({
  name: "a'b",
  value: "c",
  expected: {
    name: "a'b",
    value: "c",
  },
  description: "single quote in name",
});

formTest({
  name: "a",
  value: "b'c",
  expected: {
    name: "a",
    value: "b'c",
  },
  description: "single quote in value",
});

formTest({
  name: "a",
  value: new File([], "b'c", { type: "text/plain" }),
  expected: {
    name: "a",
    filename: "b'c",
    value: "",
  },
  description: "single quote in filename",
});

formTest({
  name: "a\\b",
  value: "c",
  expected: {
    name: "a\\b",
    value: "c",
  },
  description: "backslash in name",
});

formTest({
  name: "a",
  value: "b\\c",
  expected: {
    name: "a",
    value: "b\\c",
  },
  description: "backslash in value",
});

formTest({
  name: "a",
  value: new File([], "b\\c", { type: "text/plain" }),
  expected: {
    name: "a",
    filename: "b\\c",
    value: "",
  },
  description: "backslash in filename",
});

formTest({
  name: "áb",
  value: "ç",
  expected: {
    name: "\xC3\xA1b",
    value: "\xC3\xA7",
  },
  description: "non-ASCII in name and value",
});

formTest({
  name: "a",
  value: new File([], "ə.txt", { type: "text/plain" }),
  expected: {
    name: "a",
    filename: "\xC9\x99.txt",
    value: "",
  },
  description: "non-ASCII in filename",
});

formTest({
  name: "aəb",
  value: "c\uFFFDd",
  formEncoding: "windows-1252",
  expected: {
    name: "a&#601;b",
    value: "c&#65533;d",
  },
  description: "characters not in encoding in name and value",
});

formTest({
  name: "á",
  value: new File([], "💩", { type: "text/plain" }),
  formEncoding: "windows-1252",
  expected: {
    name: "\xE1",
    filename: "&#128169;",
    value: "",
  },
  description: "character not in encoding in filename",
});

formTest({
  name: "\uD800",
  value: "\uD800",
  formEncoding: "windows-1252",
  expected: {
    name: "&#65533;",
    value: "&#65533;"
  },
  description: "lone surrogate in name and value",
});
