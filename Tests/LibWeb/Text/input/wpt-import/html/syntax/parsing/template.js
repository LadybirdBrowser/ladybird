    /*
     * Template code
     *
     * A template is just a javascript structure. An element is represented as:
     *
     * [tag_name, {attr_name:attr_value}, child1, child2]
     *
     * the children can either be strings (which act like text nodes), other templates or
     * functions (see below)
     *
     * A text node is represented as
     *
     * ["{text}", value]
     *
     * String values have a simple substitution syntax; ${foo} represents a variable foo.
     *
     * It is possible to embed logic in templates by using a function in a place where a
     * node would usually go. The function must either return part of a template or null.
     *
     * In cases where a set of nodes are required as output rather than a single node
     * with children it is possible to just use a list
     * [node1, node2, node3]
     *
     * Usage:
     *
     * render(template, substitutions) - take a template and an object mapping
     * variable names to parameters and return either a DOM node or a list of DOM nodes
     *
     * substitute(template, substitutions) - take a template and variable mapping object,
     * make the variable substitutions and return the substituted template
     *
     */

    function is_single_node(template)
    {
        return typeof template[0] === "string";
    }

    function substitute(template, substitutions)
    {
        if (typeof template === "function") {
            var replacement = template(substitutions);
            if (replacement)
            {
                var rv = substitute(replacement, substitutions);
                return rv;
            }
            else
            {
                return null;
            }
        }
        else if (is_single_node(template))
        {
            return substitute_single(template, substitutions);
        }
        else
        {
            return filter(map(template, function(x) {
                                  return substitute(x, substitutions);
                              }), function(x) {return x !== null;});
        }
    }
    expose(substitute, "template.substitute");

    function substitute_single(template, substitutions)
    {
        var substitution_re = /\${([^ }]*)}/g;

        function do_substitution(input) {
            var components = input.split(substitution_re);
            var rv = [];
            for (var i=0; i<components.length; i+=2)
            {
                rv.push(components[i]);
                if (components[i+1])
                {
                    rv.push(substitutions[components[i+1]]);
                }
            }
            return rv;
        }

        var rv = [];
        rv.push(do_substitution(String(template[0])).join(""));

        if (template[0] === "{text}") {
            substitute_children(template.slice(1), rv);
        } else {
            substitute_attrs(template[1], rv);
            substitute_children(template.slice(2), rv);
        }

        function substitute_attrs(attrs, rv)
        {
            rv[1] = {};
            for (name in template[1])
            {
                if (attrs.hasOwnProperty(name))
                {
                    var new_name = do_substitution(name).join("");
                    var new_value = do_substitution(attrs[name]).join("");
                    rv[1][new_name] = new_value;
                };
            }
        }

        function substitute_children(children, rv)
        {
            for (var i=0; i<children.length; i++)
            {
                if (children[i] instanceof Object) {
                    var replacement = substitute(children[i], substitutions);
                    if (replacement !== null)
                    {
                        if (is_single_node(replacement))
                        {
                            rv.push(replacement);
                        }
                        else
                        {
                            extend(rv, replacement);
                        }
                    }
                }
                else
                {
                    extend(rv, do_substitution(String(children[i])));
                }
            }
            return rv;
        }

        return rv;
    }

    function make_dom_single(template)
    {
        if (template[0] === "{text}")
        {
            var element = document.createTextNode("");
            for (var i=1; i<template.length; i++)
            {
                element.data += template[i];
            }
        }
        else
        {
            var element = document.createElement(template[0]);
            for (name in template[1]) {
                if (template[1].hasOwnProperty(name))
                {
                    element.setAttribute(name, template[1][name]);
                }
            }
            for (var i=2; i<template.length; i++)
            {
                if (template[i] instanceof Object)
                {
                    var sub_element = make_dom(template[i]);
                    element.appendChild(sub_element);
                }
                else
                {
                    var text_node = document.createTextNode(template[i]);
                    element.appendChild(text_node);
                }
            }
        }

        return element;
    }



    function make_dom(template, substitutions)
    {
        if (is_single_node(template))
        {
            return make_dom_single(template);
        }
        else
        {
            return map(template, function(x) {
                           return make_dom_single(x);
                       });
        }
    }

    function render(template, substitutions)
    {
        return make_dom(substitute(template, substitutions));
    }
    expose(render, "template.render");

function expose(object, name)
{
  var components = name.split(".");
  var target = window;
  for (var i=0; i<components.length - 1; i++)
  {
    if (!(components[i] in target))
    {
      target[components[i]] = {};
    }
    target = target[components[i]];
  }
  target[components[components.length - 1]] = object;
}

function extend(array, items)
{
  Array.prototype.push.apply(array, items);
}