var namespaces = {
  "html":"http://www.w3.org/1999/xhtml",
  "math":"http://www.w3.org/1998/Math/MathML",
  "mathml":"http://www.w3.org/1998/Math/MathML",
  "svg":"http://www.w3.org/2000/svg",
  "xlink":"http://www.w3.org/1999/xlink",
  "xml":"http://www.w3.org/XML/1998/namespace",
  "xmlns":"http://www.w3.org/2000/xmlns/"
};

var prefixes = {};
for (var prefix in namespaces) {
  if (namespaces.hasOwnProperty(prefix)) {
    prefixes[namespaces[prefix]] = prefix;
  }
}
prefixes[namespaces["mathml"]] = "math";

function format(format_string) {
  var insertions = Array.prototype.slice.call(arguments, 1);
  var regexp = /%s/g;
  var match_count = 0;
  var rv = format_string.replace(regexp, function(match) {
                                   var rv = insertions[match_count];
                                   match_count++;
                                   return rv;
                                 });
  return rv;
}

function test_serializer(element) {
  element.normalize();
  var lines = [];
  function serialize_element(element, indent) {
    var indent_spaces = (new Array(indent)).join(" ");
    switch(element.nodeType) {
      case Node.DOCUMENT_TYPE_NODE:
        if (element.name) {
          if (element.publicId || element.systemId) {
            var publicId = element.publicId ? element.publicId : "";
            var systemId = element.systemId ? element.systemId : "";
            lines.push(format("|%s<!DOCTYPE %s \"%s\" \"%s\">", indent_spaces,
                                element.name, publicId, systemId));
          } else {
            lines.push(format("|%s<!DOCTYPE %s>", indent_spaces,
                                element.name));
          }
        } else {
          lines.push(format("|%s<!DOCTYPE >", indent_spaces));
        }
        break;
      case Node.DOCUMENT_NODE:
        lines.push("#document");
        break;
      case Node.DOCUMENT_FRAGMENT_NODE:
        lines.push("#document-fragment");
        break;
      case Node.COMMENT_NODE:
        lines.push(format("|%s<!-- %s -->", indent_spaces, element.nodeValue));
      break;
      case Node.TEXT_NODE:
        lines.push(format("|%s\"%s\"", indent_spaces, element.nodeValue));
        break;
      case Node.ELEMENT_NODE:
        if (element.getAttribute("data-skip") !== null) {
          return;
        }
        if (element.namespaceURI !== null && element.namespaceURI !== namespaces.html) {
          var name = format("%s %s", prefixes[element.namespaceURI],
                            element.localName);
        } else {
          var name = element.localName;
        }
        lines.push(format("|%s<%s>", indent_spaces, name));

        var attributes = Array.prototype.map.call(
         element.attributes,
         function(attr) {
           var name = (attr.namespaceURI ? prefixes[attr.namespaceURI] + " " : "") +
            attr.localName;
           return [name, attr.value];
         });
        attributes.sort(function (a, b) {
                          var x = a[0];
                          var y = b[0];
                          if (x === y) {
                            return 0;
                          }
                          return x > y ? 1 : -1;
                        });

        attributes.forEach(
          function(attr) {
            var indent_spaces = (new Array(indent + 2)).join(" ");
            lines.push(format("|%s%s=\"%s\"", indent_spaces, attr[0], attr[1]));
          }
        );
        if ("HTMLTemplateElement" in window &&
            Object.prototype.toString.call(element) === "[object HTMLTemplateElement]") {
          indent += 2;
          indent_spaces = (new Array(indent)).join(" ");
          lines.push(format("|%scontent", indent_spaces));
          indent += 2;
          Array.prototype.forEach.call(element.content.childNodes,
                                       function(node) {
                                         serialize_element(node, indent);
                                       });
          indent -= 4;
        }
        break;
    }
    indent += 2;
    Array.prototype.forEach.call(element.childNodes,
                                 function(node) {
                                   serialize_element(node, indent);
                                 });
  }
  serialize_element(element, 0);
  return lines.join("\n");
}

function parse_query() {
    var query = location.search.slice(1);
    var vars = query.split("&");
    var fields = vars.map(function (x) {
                            var split = x.split("=");
                            return [split[0], split.slice(1).join("=")];
                          });
    return fields;
}

function get_type() {
  var run_type = "uri";
  var fields = parse_query();
  fields.forEach(function(x) {
                   if(x[0] == "run_type") {
                     run_type = x[1];
                   }
                 });
  return run_type;
};

var test_in_blob_uri = get_test_func(function (iframe, uri_encoded_input, t) {
                                       var b = new Blob([decodeURIComponent(uri_encoded_input)], { type: "text/html" });
                                       var blobURL = URL.createObjectURL(b);
                                       iframe.src = blobURL;
                                       t.add_cleanup(function() {
                                         URL.revokeObjectURL(blobURL);
                                       });
                                     });

var test_document_write = get_test_func(function(iframe, uri_encoded_input, t) {
                                          iframe.contentDocument.open();
                                          var input = decodeURIComponent(uri_encoded_input);
                                          iframe.contentDocument.write(input);
                                          iframe.contentDocument.close();
                                        });

var test_document_write_single = get_test_func(function(iframe, uri_encoded_input, t) {
                                                 iframe.contentDocument.open();
                                                 var input = decodeURIComponent(uri_encoded_input);
                                                 for (var i=0; i< input.length; i++) {
                                                   iframe.contentDocument.write(input[i]);
                                                 }
                                                 iframe.contentDocument.close();
                                               });

function get_test_func(inject_func) {
  function test_func(iframe, t, test_id, uri_encoded_input, escaped_expected) {
    var expected = decodeURIComponent(escaped_expected);
    current_tests[iframe.id] = {test_id:test_id,
                                uri_encoded_input:uri_encoded_input,
                                expected:expected,
                                actual:null
                               };

    iframe.onload = function() {
      t.step(function() {
               iframe.onload = null;
               var serialized_dom = test_serializer(iframe.contentDocument);
               current_tests[iframe.id].actual = serialized_dom;
               assert_equals(serialized_dom, expected);
               t.done();
             }
            );
    };
    inject_func(iframe, uri_encoded_input, t);
  }
  return test_func;
}

function test_fragment(iframe, t, test_id, uri_encoded_input, escaped_expected, container) {
  var input_string = decodeURIComponent(uri_encoded_input);
  var expected = decodeURIComponent(escaped_expected);
  current_tests[iframe.id] = {
      test_id:test_id,
      input:uri_encoded_input,
      expected:expected,
      actual:null,
      container:container
  };

  var components = container.split(" ");
  var container_elem = null;
  if (components.length > 1) {
    var namespace = namespaces[components[0]];
    container_elem = document.createElementNS(namespace,
                                              components[0] + ":" +
                                              components[1]);
  } else {
     container_elem = document.createElement(container);
  }
  container_elem.innerHTML = input_string;
  var serialized_dom;
  if (container_elem.namespaceURI === namespaces["html"] && container_elem.localName === "template") {
    serialized_dom = test_serializer(container_elem.content);
  } else {
    serialized_dom = test_serializer(container_elem);
  }
  current_tests[iframe.id].actual = serialized_dom;
  serialized_dom = convert_innerHTML(serialized_dom);
  assert_equals(serialized_dom, expected);
  t.done();
}

function convert_innerHTML(serialized_dom) {
  var lines = serialized_dom.split("\n");
  assert_not_equals(lines[0], "<template>", "template is never the innerHTML context object");
  lines[0] = "#document";
  return lines.join("\n");
}

function print_diffs(test_id, uri_encoded_input, expected, actual, container) {
  container = container ? container : null;
  if (actual) {
    var diffs = mark_diffs(expected, actual);
    var expected_text = diffs[0];
    var actual_text = diffs[1];
  } else {
    var expected_text = expected;
    var actual_text = "";
  }

  var tmpl = ["div", {"id":"${test_id}"},
              ["h2", {}, "${test_id}"],
              function(vars) {
                if (vars.container !== null) {
                  return ["div", {"class":"container"},
                  ["h3", {}, "innerHTML Container"],
                  ["pre", {}, vars.container]];
                } else {
                  return null;
                }
              },
              ["div", {"id":"input_${test_id}"}, ["h3", {}, "Input"], ["pre", {},
                                                                       ["code", {}, decodeURIComponent(uri_encoded_input)]]],
              ["div", {"id":"expected_${test_id}"}, ["h3", {}, "Expected"],
               ["pre", {}, ["code", {}, expected_text]]],
              ["div", {"id":"actual_${test_id}"}, ["h3", {}, "Actual"],
               ["pre", {}, ["code", {}, actual_text]]]
             ];

  var diff_dom = template.render(tmpl, {test_id:test_id, container:container});
  document.body.appendChild(diff_dom);
}

var current_tests = {};
var iframe_map = {};

function init_tests(test_type) {
  var test_func = null;
  var test_funcs = {
    "write":test_document_write,
    "write_single":test_document_write_single,
    "uri":test_in_blob_uri,
    "innerHTML":test_fragment
  };
  var tests_started = 0;
  var tests_complete = 0;

  setup(function() {
    test_func = test_funcs[test_type];

    var fails = [];

    add_result_callback(function(test) {
      tests_complete++;
      var iframe = document.getElementById(iframe_map[test.name]);
      if (test.status !== test.PASS) {
        fails.push(current_tests[iframe.id]);
        var new_iframe = document.createElement("iframe");
        new_iframe.style.display = "none";
        new_iframe.id = iframe.id;
        document.body.replaceChild(new_iframe, iframe);
        iframe = new_iframe;
      }
      if (tests_complete === order.length) {
        done();
      } else if (tests_started < order.length) {
        test_next(iframe);
      }
    });

    add_completion_callback(function() {
      fails.forEach(function(t) {
                      print_diffs(t.test_id, t.uri_encoded_input,
                      t.expected, t.actual);
                    });
      });

    //Create the iframes we will use to test
    //in the innerHTML case these are not actually used
    //but it is convenient to reuse the same code
    for (var i=0; i<num_iframes; i++) {
      var iframe = document.createElement("iframe");
      iframe.id = "iframe_" + i;
      iframe.style.display = "none";
      document.body.appendChild(iframe);
    }
  },
  {explicit_done:true});

  function test_next(iframe) {
    var test_id = order[tests_started];
    tests_started++;
    var x = tests[test_id];
    var t = x[0];
    iframe_map[t.name] = iframe.id;
    step_timeout(function() {
                 t.step(function() {
                   var string_uri_encoded_input = x[1];
                   var string_escaped_expected = x[2];
                   if (test_type === "innerHTML") {
                     var container = x[3];
                   }
                   test_func(iframe, t, test_id, string_uri_encoded_input, string_escaped_expected,
                             container);
                 });
         }, 0);
  }

  onload = function() {
    Array.prototype.forEach.call(document.getElementsByTagName("iframe"),
    function(iframe) {
      if (tests_started<order.length) {
        test_next(iframe);
      }
    });
  };
}
