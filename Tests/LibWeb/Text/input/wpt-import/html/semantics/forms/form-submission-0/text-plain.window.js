// META: script=enctypes-helper.js

const formTest = formSubmissionTemplate("text/plain");

formTest({
  name: "basic",
  value: "test",
  expected: "basic=test\r\n",
  description: "Basic test",
});

formTest({
  name: "basic",
  value: new File([], "file-test.txt"),
  expected: "basic=file-test.txt\r\n",
  description: "Basic File test",
});

formTest({
  name: "a\0b",
  value: "c",
  expected: "a\0b=c\r\n",
  description: "0x00 in name",
});

formTest({
  name: "a",
  value: "b\0c",
  expected: "a=b\0c\r\n",
  description: "0x00 in value",
});

formTest({
  name: "a",
  value: new File([], "b\0c"),
  expected: "a=b\0c\r\n",
  description: "0x00 in filename",
});

formTest({
  name: "a\nb",
  value: "c",
  expected: "a\r\nb=c\r\n",
  description: "\\n in name",
});

formTest({
  name: "a\rb",
  value: "c",
  expected: "a\r\nb=c\r\n",
  description: "\\r in name",
});

formTest({
  name: "a\r\nb",
  value: "c",
  expected: "a\r\nb=c\r\n",
  description: "\\r\\n in name",
});

formTest({
  name: "a\n\rb",
  value: "c",
  expected: "a\r\n\r\nb=c\r\n",
  description: "\\n\\r in name",
});

formTest({
  name: "a",
  value: "b\nc",
  expected: "a=b\r\nc\r\n",
  description: "\\n in value",
});

formTest({
  name: "a",
  value: "b\rc",
  expected: "a=b\r\nc\r\n",
  description: "\\r in value",
});

formTest({
  name: "a",
  value: "b\r\nc",
  expected: "a=b\r\nc\r\n",
  description: "\\r\\n in value",
});

formTest({
  name: "a",
  value: "b\n\rc",
  expected: "a=b\r\n\r\nc\r\n",
  description: "\\n\\r in value",
});

formTest({
  name: "a",
  value: new File([], "b\nc"),
  expected: "a=b\r\nc\r\n",
  description: "\\n in filename",
});

formTest({
  name: "a",
  value: new File([], "b\rc"),
  expected: "a=b\r\nc\r\n",
  description: "\\r in filename",
});

formTest({
  name: "a",
  value: new File([], "b\r\nc"),
  expected: "a=b\r\nc\r\n",
  description: "\\r\\n in filename",
});

formTest({
  name: "a",
  value: new File([], "b\n\rc"),
  expected: "a=b\r\n\r\nc\r\n",
  description: "\\n\\r in filename",
});

formTest({
  name: 'a"b',
  value: "c",
  expected: 'a"b=c\r\n',
  description: "double quote in name",
});

formTest({
  name: "a",
  value: 'b"c',
  expected: 'a=b"c\r\n',
  description: "double quote in value",
});

formTest({
  name: "a",
  value: new File([], 'b"c'),
  expected: 'a=b"c\r\n',
  description: "double quote in filename",
});

formTest({
  name: "a'b",
  value: "c",
  expected: "a'b=c\r\n",
  description: "single quote in name",
});

formTest({
  name: "a",
  value: "b'c",
  expected: "a=b'c\r\n",
  description: "single quote in value",
});

formTest({
  name: "a",
  value: new File([], "b'c"),
  expected: "a=b'c\r\n",
  description: "single quote in filename",
});

formTest({
  name: "a\\b",
  value: "c",
  expected: "a\\b=c\r\n",
  description: "backslash in name",
});

formTest({
  name: "a",
  value: "b\\c",
  expected: "a=b\\c\r\n",
  description: "backslash in value",
});

formTest({
  name: "a",
  value: new File([], "b\\c"),
  expected: "a=b\\c\r\n",
  description: "backslash in filename",
});

formTest({
  name: "áb",
  value: "ç",
  expected: "\xC3\xA1b=\xC3\xA7\r\n",
  description: "non-ASCII in name and value",
});

formTest({
  name: "a",
  value: new File([], "ə.txt"),
  expected: "a=\xC9\x99.txt\r\n",
  description: "non-ASCII in filename",
});

formTest({
  name: "aəb",
  value: "c\uFFFDd",
  formEncoding: "windows-1252",
  expected: "a&#601;b=c&#65533;d\r\n",
  description: "characters not in encoding in name and value",
});

formTest({
  name: "á",
  value: new File([], "💩"),
  formEncoding: "windows-1252",
  expected: "\xE1=&#128169;\r\n",
  description: "character not in encoding in filename",
});

formTest({
  name: "\uD800",
  value: "\uD800",
  formEncoding: "windows-1252",
  expected: "&#65533;=&#65533;\r\n",
  description: "lone surrogate in name and value",
});
