# Issue-reporting guidelines

Report problems using our detailed [bug-reporting
form](https://github.com/LadybirdBrowser/ladybird/issues/new?template=bug_report.yml), which helps to ensure that, for us to reproduce and investigate the problem, your bug report includes the information needed — including a reduced test case.

## How you can write a reduced test case

When raising an issue, the single-most important thing you can provide is a reduced test others can use to reproduce the problem.

You can create a reduced test case for a problem by starting from the HTML/JS/CSS source for a particular website/page which causes the problem, and removing as much content as possible while still having a document that reproduces the problem.

Here’s how you can do that:

1. Create a local `REDUCTION.html` (or whatever name) copy of the page — ideally by using something like the [SingleFile](https://addons.mozilla.org/en-US/firefox/addon/single-file/) extension for Firefox or Chrome to save a canned copy of the page as it currently exists (post-JS-execution).

   That also allows you to use Firefox/Chrome devtools to quickly strip out irrelevant elements from the page before saving a copy.

2. If you’re *not* using something like SingleFile, then: To ensure any images,external style sheets, or external scripts that use a relative path will get loaded by your local `REDUCTION.html` document, put a `base` element into the document — like this:

   ```html
   <base href="https://ladybird.org/">
   ```

    However, if the problem appears to be caused not by anything in the source of the document itself, but instead by something in an external script or external stylesheet, then you’ll also need to create a local copy of the problem script or problem stylesheet.

3. Open/load the `REDUCTION.html` file in Ladybird, and verify that the same problem occurs with it as occurs with the original website/page you copied it from.

4. **Script-related problems:** Especially if you believe the problem is related to any JavaScript that the document is executing, then temporarily disable scripting by unchecking **Enable Scripting** option in the **Debug** menu in Ladybird, and then reload the `REDUCTION.html` file in Ladybird.

   * If the problem still happens after you’ve disabled scripting, then you can remove any and all `script` elements from the document, and you can continue on from there.

   * If the problem does not happen any longer after you’ve disabled scripting, then that tells you the cause is related to the contents from one or more of the `script` elements, and you can check the **Enable Scripting** option to turn scripting back on, and you can continue on from there.

5. **CSS-related problems**: Especially if you believe that the problem is related to any CSS stylesheets the document is using, then:

   1. [If you’ve not used something like the [SingleFile](https://addons.mozilla.org/en-US/firefox/addon/single-file/) extension for Firefox or Chrome] Add a `style` element in the `REDUCTION.html` file, and then for each external style sheet the document has, paste the contents from that external stylesheet into that `style` element.

   2. Start removing CSS rules from any and all `style` elements in the document, and reload the `REDUCTION.html` file in Ladybird.

      * If the problem does not happen any longer after you’ve removed a particular CSS rule, then you may have isolated the cause. Re-add that CSS rule to the document, and continue on from there.

      * If the problem does continue to happen after you’ve removed a CSS rule, then you’ve successfully reduced the test case by one CSS rule, and you can continue on from there.

   Either way, keep repeating the two steps above, removing CSS rules one by one, and with each removal, checking whether the problem still happens — and if it does, putting the CSS rule you removed back into the document, or otherwise, leaving the CSS rule out and moving on.

6. **HTML-related problems:** Begin removing one *element* at time from the `REDUCTION.html` file, starting with the elements in the document `head`.

7. Reload the `REDUCTION.html` file in Ladybird, and verify whether the problem is still happening.

   * If the problem does not happen any longer after you’ve removed a particular element, then you may have isolated the cause. Re-add that element to the document, and continue on from there.

   * If the problem does continue to happen after you’ve removed a particular element, then you’ve successfully reduced the test case by one element, and you can continue on from there.

   Either way, keep repeating the steps: removing elements one by one, and with each removal, checking whether the problem still happens — and if it does, putting the element you removed back into the document, or otherwise, leaving the element out and moving on.

In the end, after following the steps above and removing elements and CSS rules, you’ll have a reduced test case that you can include when you raise an issue for the problem.

## How to share/publish your reduced test case at a URL

You can take your reduced test case and post it online at a site such as the following:

* https://codepen.io/pen/
* https://jsbin.com
* https://jsfiddle.net

That will give you a URL which you can then include in the issue you raise for the problem.

*[Credits: The “How you can write a reduced test case” details above are largely taken from https://webkit.org/test-case-reduction/.]*
