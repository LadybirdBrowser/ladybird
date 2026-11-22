var namespaces = {
    "html":"http://www.w3.org/1999/xhtml",
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
  //element.normalize();
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
      case Node.PROCESSING_INSTRUCTION_NODE:
        lines.push(format("|%s<?%s %s>", indent_spaces, element.target, element.data));
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

function runTests(tests) {
    tests.forEach(function(test){
        var expected = decodeURIComponent(test.expected);
        var t = async_test(document.title + ' - ' + test.name);
        t.step(function(){
            var video = document.createElement('video');
            var track = document.createElement('track');
            assert_true('src' in track, 'track not supported');
            t.test_id = test.name;
            t.url_encoded_input = test.input;
            t.expected = expected;
            var track_blob = new Blob(['WEBVTT\n\n00:00.000 --> 00:01.000\n',
                                       decodeURIComponent(test.input)],
                                      { type: 'text/vtt' });
            var track_url = URL.createObjectURL(track_blob);;
            track.src = track_url;
            t.add_cleanup(function() {
                URL.revokeObjectURL(track_url);
            });
            track['default'] = true;
            track.kind = 'subtitles';
            track.onload = t.step_func(trackLoaded);
            track.onerror = t.step_func(trackError);
            video.appendChild(track);
            document.body.appendChild(video);
        });
    });
}

function trackLoaded(e) {
    var track = e.target;
    setTimeout(removeElm, 0, track.parentNode);
    var cue = track.track.cues[0];
    var frag = cue.getCueAsHTML();
    var got = test_serializer(frag);
    if (got !== this.expected) {
        print_diffs(this.test_id, this.url_encoded_input, this.expected, got);
    }
    assert_equals(got, this.expected);
    this.done();
}

function trackError(e) {
    setTimeout(removeElm, 0, e.target.parentNode);
    assert_unreached('got error event');
    this.done();
}

function removeElm(elm) {
    document.body.removeChild(elm);
}
