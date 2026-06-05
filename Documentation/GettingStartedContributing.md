# Getting involved with Ladybird

Welcome to the Ladybird web browser project!

This document is a beginner-friendly guide to getting familiar with Ladybird and helping the project through testing, bug reports, reductions, standards discussion, design discussion, security reports, and technical feedback.

## Getting familiar with the project

Ladybird is a large project making use of many homegrown and third-party libraries, written primarily in C++.

It is recommended you read the README and FAQs in case they already answer any questions you have:

* [README](/README.md)
* [FAQ](FAQ.md)
* [FAQ on the Ladybird website](https://ladybird.org/#faq)

The [Discord server](https://discord.gg/nvfjVJ4Svh) is the preferred way to get in contact with the maintainers and community.

## Getting familiar with browser engineering in general

If you’ve never worked on browser-engine code before, and you’re not sure where to begin — one great place to get started is by reading the book [Web Browser Engineering](https://browser.engineering/). It explains how browser engines in general work, and how they’re built — by walking you through real code for actually building all the parts of a basic but complete browser engine (networking code, HTML parsing, layout engine, JavaScript handling, and more), in a couple thousand lines of Python.

## Building the code

Ladybird must be built from source during this pre-alpha stage of development, and currently natively supports Linux and macOS; running it on Windows requires WSL.

Carefully follow the steps outlined in the [build instructions](BuildInstructionsLadybird.md). If you are facing issues, consult the [troubleshooting guide](Troubleshooting.md) and the #build-problems channel on Discord.

## Finding bugs and other issues

Here are some of the ways you can find an issue in Ladybird:

* By checking the [issue tracker](https://github.com/LadybirdBrowser/ladybird/issues).
* Manually, by using the browser as you normally would.
* By finding failing WPT tests on [WPT.fyi](https://wpt.fyi/results/?label=master&product=ladybird). You do not need to submit issue reports for individual tests.
* By finding WPT tests on [WPT.fyi](https://wpt.fyi/results/?label=master&product=ladybird) that are [timing out in Ladybird](https://wpt.fyi/results/?product=ladybird&q=status%3Atimeout). For a real-world walk-through of doing that from start to finish with an actual timing-out-in-Ladybird test case, see the [“Fixing a WPT timeout in Window.postMessage()”](https://www.youtube.com/watch?v=X4S9afzRTXs) “browser hacking” video.
* By using a profiling tool such as [Callgrind](https://valgrind.org/docs/manual/cl-manual.html) to find code that can be improved.
* By looking for [`TODO`](https://github.com/search?q=repo%3ALadybirdBrowser%2Fladybird%20%22%2F%2F%20TODO%22&type=code) and [`FIXME`](https://github.com/search?q=repo%3ALadybirdBrowser%2Fladybird+%22%2F%2F+FIXME%22&type=code) comments in the codebase.

If you’re not necessarily fluent in C++, beginning by troubleshooting WPT tests may be the very best way to get started helping the project — especially if you _do_ already have some proficiency with frontend JavaScript code.

That’s because without even knowing any C++ at all, you can still — by working just with the JavaScript code in the WPT test source — get a long way toward isolating the cause of a particular WPT test failure or timeout. That alone can be a very big help to maintainers and other people investigating the related C++ code.

## Submitting an issue

If you have found an issue that is not already in the [issue tracker](https://github.com/LadybirdBrowser/ladybird/issues), you may submit it. Do not submit general questions about the project, please use the Discord server instead.

Read the [project participation guidelines](/CONTRIBUTING.md), in particular the Issue policy and Human language policy. It is recommended you reduce the website to the most minimal amount of HTML/CSS/JS which still results in the error (if applicable), and provide the result expected from other browsers vs Ladybird. Read the [detailed issue-reporting guidelines](/ISSUES.md) for the exact steps to do so.

## Learning the codebase

### Getting familiar with C++

Ladybird requires at least a basic knowledge of C++. Consult a tutorial website like [Learn C++](https://www.learncpp.com/) and online references if you need help.

### Getting familiar with the codebase

Ladybird makes use of the included AK library instead of the C++ STL, and employs a specific coding style based around it. Unfortunately most AK and internal library facilities are not documented; you may need to look for the header files, and examples of usage in existing code.

Developer documentation:

* [Coding style](CodingStyle.md)
* [Coding patterns](Patterns.md)
* [Smart pointers](SmartPointers.md)
* [String formatting](StringFormatting.md)
