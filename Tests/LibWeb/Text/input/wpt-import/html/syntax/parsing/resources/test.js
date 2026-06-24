// Runner for html5lib-tests .dat files.
//
// Each wrapper page (html5lib_url.html, html5lib_write.html, …) loads this
// script with `<script src="resources/test.js" data-run-type="…"></script>`
// and lists each .dat file as `<meta name=variant content="?file=NAME">`.
// We fetch resources/NAME.dat, parse it, and run each entry: document tests
// parse via the page's run_type; entries with #document-fragment always parse
// via innerHTML.

(() => {
  "use strict";

  const NAMESPACES = {
    html:   "http://www.w3.org/1999/xhtml",
    math:   "http://www.w3.org/1998/Math/MathML",
    mathml: "http://www.w3.org/1998/Math/MathML",
    svg:    "http://www.w3.org/2000/svg",
    xlink:  "http://www.w3.org/1999/xlink",
    xml:    "http://www.w3.org/XML/1998/namespace",
    xmlns:  "http://www.w3.org/2000/xmlns/",
  };
  const PREFIXES = {};
  for (const prefix of Object.keys(NAMESPACES)) {
    PREFIXES[NAMESPACES[prefix]] = prefix;
  }
  PREFIXES[NAMESPACES.mathml] = "math";

  function serializeTree(root) {
    root.normalize();
    const lines = [];
    // The .dat #document format prefixes each line with "|" + N spaces of
    // indent, where the document itself has no "|" line, its direct children
    // get 1 space, their children get 3, then 5, 7, … (i.e. 2*depth - 1).
    const walk = (node, depth) => {
      const pad = depth > 0 ? " ".repeat(2 * depth - 1) : "";
      const innerPad = " ".repeat(2 * depth + 1);
      switch (node.nodeType) {
        case Node.DOCUMENT_TYPE_NODE:
          if (node.name) {
            if (node.publicId || node.systemId) {
              lines.push(`|${pad}<!DOCTYPE ${node.name} "${node.publicId ?? ""}" "${node.systemId ?? ""}">`);
            } else {
              lines.push(`|${pad}<!DOCTYPE ${node.name}>`);
            }
          } else {
            lines.push(`|${pad}<!DOCTYPE >`);
          }
          break;
        case Node.DOCUMENT_NODE:
          lines.push("#document");
          break;
        case Node.DOCUMENT_FRAGMENT_NODE:
          lines.push("#document-fragment");
          break;
        case Node.COMMENT_NODE:
          lines.push(`|${pad}<!-- ${node.nodeValue} -->`);
          break;
        case Node.TEXT_NODE:
          lines.push(`|${pad}"${node.nodeValue}"`);
          break;
        case Node.ELEMENT_NODE: {
          if (node.getAttribute("data-skip") !== null) return;
          const tag = (node.namespaceURI && node.namespaceURI !== NAMESPACES.html)
            ? `${PREFIXES[node.namespaceURI]} ${node.localName}`
            : node.localName;
          lines.push(`|${pad}<${tag}>`);
          const attrs = [...node.attributes].map(a => [
            (a.namespaceURI ? PREFIXES[a.namespaceURI] + " " : "") + a.localName,
            a.value,
          ]);
          attrs.sort((a, b) => a[0] === b[0] ? 0 : (a[0] > b[0] ? 1 : -1));
          for (const [name, value] of attrs) {
            lines.push(`|${innerPad}${name}="${value}"`);
          }
          if (node.namespaceURI === NAMESPACES.html && node.localName === "template") {
            lines.push(`|${innerPad}content`);
            for (const child of node.content.childNodes) walk(child, depth + 2);
          }
          break;
        }
      }
      for (const child of node.childNodes) walk(child, depth + 1);
    };
    walk(root, 0);
    return lines.join("\n");
  }

  // Per-flavor runners. Each fills `iframe` with the parser's output, then
  // serializes and asserts. record() captures input/expected/actual on the
  // shared map so failure diffs can be rendered after the run.
  function makeDocRunner(inject) {
    return ({ iframe, t, id, input, expected, record }) => {
      record({ id, input, expected, actual: null });
      iframe.onload = () => t.step(() => {
        iframe.onload = null;
        const actual = serializeTree(iframe.contentDocument);
        record({ id, input, expected, actual });
        assert_equals(actual, expected);
        t.done();
      });
      inject(iframe, input, t);
    };
  }

  const RUNNERS = {
    url: makeDocRunner((iframe, input, t) => {
      const blob = new Blob([input], { type: "text/html" });
      const url = URL.createObjectURL(blob);
      iframe.src = url;
      t.add_cleanup(() => URL.revokeObjectURL(url));
    }),
    write: makeDocRunner((iframe, input) => {
      iframe.contentDocument.open();
      iframe.contentDocument.write(input);
      iframe.contentDocument.close();
    }),
    write_single: makeDocRunner((iframe, input) => {
      iframe.contentDocument.open();
      for (const ch of input) iframe.contentDocument.write(ch);
      iframe.contentDocument.close();
    }),
    innerHTML: ({ iframe, t, id, input, expected, container, record }) => {
      const parts = container.split(" ");
      const containerEl = parts.length > 1
        ? document.createElementNS(NAMESPACES[parts[0]], `${parts[0]}:${parts[1]}`)
        : document.createElement(container);
      containerEl.innerHTML = input;
      const root = (containerEl.namespaceURI === NAMESPACES.html && containerEl.localName === "template")
        ? containerEl.content : containerEl;
      let actual = serializeTree(root);
      record({ id, input, expected, actual, container });
      // serializeTree emits "#document-fragment"; the .dat expected tree uses
      // "#document" as its root marker.
      const lines = actual.split("\n");
      assert_not_equals(lines[0], "<template>", "template is never the innerHTML context object");
      lines[0] = "#document";
      actual = lines.join("\n");
      assert_equals(actual, expected);
      t.done();
    },
  };

  // .dat format (html5lib-tests):
  //   #data            input HTML
  //   #errors / #new-errors / #errors-new  parsing errors (ignored here)
  //   #script-on / #script-off            scripting requirement (we honor #script-off)
  //   #document-fragment NAME             context element for innerHTML
  //   #document                           expected serialized tree
  // This mirrors the upstream html5lib-python TestData parser:
  // each section accumulates raw lines (with trailing \n), and at every test
  // boundary the active section gets one extra char clipped (the blank-line
  // separator) before the per-section trailing-\n normalization runs.
  function parseDat(text) {
    if (text.endsWith("\n")) text = text.slice(0, -1);
    const fileLines = text.split("\n");
    const cases = [];
    let data = null;
    let key = null;

    const isSectionHeading = line => {
      if (!line.startsWith("#")) return null;
      const heading = line.slice(1).trim();
      return heading || null;
    };
    const normalise = obj => {
      const out = {};
      for (const k of Object.keys(obj)) {
        out[k] = obj[k].endsWith("\n") ? obj[k].slice(0, -1) : obj[k];
      }
      return out;
    };

    for (let i = 0; i < fileLines.length; i++) {
      const line = fileLines[i] + (i === fileLines.length - 1 ? "" : "\n");
      const heading = isSectionHeading(line);
      if (heading) {
        if (data && heading === "data") {
          if (key !== null && data[key].length > 0) {
            data[key] = data[key].slice(0, -1);
          }
          cases.push(normalise(data));
          data = null;
        }
        if (data === null) data = {};
        key = heading;
        data[key] = "";
      } else if (key !== null) {
        data[key] += line;
      }
    }
    if (data) cases.push(normalise(data));

    return cases.map(c => ({
      data: c["data"] ?? "",
      document: c["document"] ?? "",
      fragment: c["document-fragment"],
      scriptOff: "script-off" in c,
    }));
  }

  function printDiff({ id, input, expected, actual, container }) {
    const [expectedText, actualText] = actual
      ? mark_diffs(expected, actual)
      : [expected, ""];
    const tmpl = ["div", { id: "${id}" },
      ["h2", {}, "${id}"],
      vars => vars.container != null
        ? ["div", { class: "container" },
            ["h3", {}, "innerHTML Container"],
            ["pre", {}, vars.container]]
        : null,
      ["div", { id: "input_${id}" }, ["h3", {}, "Input"],
        ["pre", {}, ["code", {}, input]]],
      ["div", { id: "expected_${id}" }, ["h3", {}, "Expected"],
        ["pre", {}, ["code", {}, expectedText]]],
      ["div", { id: "actual_${id}" }, ["h3", {}, "Actual"],
        ["pre", {}, ["code", {}, actualText]]],
    ];
    document.body.appendChild(template.render(tmpl, { id, container: container ?? null }));
  }

  // ----- Entry point -----

  const NUM_IFRAMES = 8;
  const runType = document.currentScript.dataset.runType;
  const file = new URLSearchParams(location.search).get("file");

  setup({ explicit_done: true });

  if (!file || !RUNNERS[runType]) {
    test(() => assert_unreached(`bad config: file=${file}, runType=${runType}`),
         "html5lib-runner setup");
    done();
    return;
  }

  fetch(`resources/${file}.dat`)
    .then(r => r.ok ? r.text() : Promise.reject(new Error(`fetch ${file}.dat: ${r.status}`)))
    .then(text => {
      const cases = parseDat(text).filter(c => !c.scriptOff);
      const entries = [];
      const seenNames = new Set();
      const records = new Map();          // iframe.id -> latest record for that test
      const iframeForTest = new Map();    // test.name -> iframe.id

      const escapeForName = s => s.replace(/[\x00-\x1f]/g, ch => {
        const code = ch.charCodeAt(0);
        if (code === 0x0a) return "\\n";
        if (code === 0x0d) return "\\r";
        if (code === 0x09) return "\\t";
        return `\\x${code.toString(16).padStart(2, "0")}`;
      });

      for (const c of cases) {
        // Fragment tests don't depend on the page's parsing mode (they always
        // go through innerHTML), so we run them in only one wrapper to avoid
        // counting the same assertion three times.
        if (c.fragment !== undefined && runType !== "url") continue;
        const name = c.fragment !== undefined
          ? `${escapeForName(c.data)} (innerHTML in ${c.fragment})`
          : escapeForName(c.data);
        // Identical (input, fragment-context) pairs would collide as test
        // names. Upstream html5lib-tests doesn't ship any (after #script-off
        // is filtered out); if that ever changes we want to know rather than
        // silently drop one.
        if (seenNames.has(name)) {
          throw new Error(`duplicate test in ${file}.dat: ${name}`);
        }
        seenNames.add(name);
        entries.push({
          t: async_test(name),
          input: c.data,
          expected: "#document\n" + c.document,
          fragment: c.fragment,
        });
      }

      // Nothing to run (e.g. fragment-only .dat in a non-url wrapper). The
      // updater shouldn't list such variants; surface a clear failure if one
      // ever does end up here so the cause is obvious.
      if (entries.length === 0) {
        throw new Error(`${file}.dat has no tests for run_type=${runType}`);
      }

      const fails = [];
      let started = 0;
      let completed = 0;

      add_result_callback(testObj => {
        completed++;
        let iframe = document.getElementById(iframeForTest.get(testObj.name));
        if (testObj.status !== testObj.PASS) {
          fails.push(records.get(iframe.id));
          const fresh = document.createElement("iframe");
          fresh.style.display = "none";
          fresh.id = iframe.id;
          document.body.replaceChild(fresh, iframe);
          iframe = fresh;
        }
        if (completed === entries.length) {
          done();
        } else if (started < entries.length) {
          runNext(iframe);
        }
      });

      add_completion_callback(() => fails.forEach(printDiff));

      for (let i = 0; i < NUM_IFRAMES; i++) {
        const iframe = document.createElement("iframe");
        iframe.id = `iframe_${i}`;
        iframe.style.display = "none";
        document.body.appendChild(iframe);
      }

      const runNext = iframe => {
        const entry = entries[started++];
        const runner = entry.fragment !== undefined ? RUNNERS.innerHTML : RUNNERS[runType];
        iframeForTest.set(entry.t.name, iframe.id);
        const record = data => records.set(iframe.id, data);
        step_timeout(() => entry.t.step(() => {
          runner({ iframe, t: entry.t, id: entry.t.name, input: entry.input, expected: entry.expected, container: entry.fragment, record });
        }), 0);
      };

      for (const iframe of document.getElementsByTagName("iframe")) {
        if (started < entries.length) runNext(iframe);
      }
    })
    .catch(err => {
      test(() => { throw err; }, `html5lib-runner: load ${file}.dat`);
      done();
    });
})();
