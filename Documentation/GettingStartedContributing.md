# Getting started contributing to Ladybird
Welcome to the Ladybird web browser project!

This document aims to be a beginner-friendly guide to your first Ladybird contribution; remember to read the linked documentation for more information.

## Getting familiar with the project
Ladybird is a large project making use of many homegrown and third-party libraries, written primarily in C++.

It is recommended you read the README and FAQs in case they already answer any questions you have:

* [README](/README.md)
* [FAQ](FAQ.md)
* [FAQ on the Ladybird website](https://ladybird.org/#faq)

The [Discord server](https://discord.gg/nvfjVJ4Svh) is the preferred way to get in contact with the maintainers and community.

## Building the code
Ladybird must be built from source during this pre-alpha stage of development, and currently natively supports Linux and macOS; running it on Windows requires WSL.

Carefully follow the steps outlined in the [build instructions](BuildInstructionsLadybird.md). If you are facing issues, consult the [troubleshooting guide](Troubleshooting.md) and the #build-problems channel on Discord.

## Finding bugs and other issues
Here are some of the ways you can find an issue in Ladybird:

* By checking the [issue tracker](https://github.com/LadybirdBrowser/ladybird/issues).
* Manually, by using the browser as you normally would.
* By finding failing WPT tests on [WPT.fyi](https://staging.wpt.fyi/results/?label=master&product=ladybird). Note that while fixes are welcome, you don't need to submit issue reports for individual tests.

There currently isn't a list of beginner-friendly issues, nor a strict roadmap of ones to address first. It is ultimately up to you to choose a task that you feel comfortable working on.

## Submitting an issue
If you have found an issue that is not already in the [issue tracker](https://github.com/LadybirdBrowser/ladybird/issues), you may submit it. Do not submit general questions about the project, please use the Discord server instead.

Read the [full contribution guidelines](/CONTRIBUTING.md), in particular the Issue policy and Human language policy. It is recommended you reduce the website to the most minimal amount of HTML/CSS/JS which still results in the error (if applicable), and provide the result expected from other browsers vs Ladybird. Read the [detailed issue-reporting guidelines](/ISSUES.md) for the exact steps to do so.

## Submitting your code
### Getting familiar with C++
Ladybird requires at least a basic knowledge of C++, consult a tutorial website like [Learn C++](https://www.learncpp.com/) and online references if you need help. Start small before attempting to make large changes, as they require more in-depth C++ knowledge.

### Getting familiar with the codebase
Ladybird makes use of the included AK library instead of the C++ STL, and employs a specific coding style based around it. Unfortunately most AK and internal library facilities are not documented; you may need to look for the header files, and examples of usage in existing code.

Developer documentation:

* [Coding patterns](Patterns.md)
* [Smart pointers](SmartPointers.md)
* [String formatting](StringFormatting.md)

### Using `git`
The recommended way to work on Ladybird is by cloning the main repository locally, then forking it on GitHub and adding your repository as a Git remote:
```sh
git remote add myfork git@github.com:MyUsername/ladybird.git
```

You can then create a new branch and start making changes to the code:
```sh
git checkout -b mybranch
```

And finally push the commits to your fork:
```sh
# ONLY run this the first time you push
git push --set-upstream myfork mybranch
# This will work for further pushes
git push
```

If your pull request is showing conflicts with the master branch, resolve them locally with:
```sh
# On mybranch
git fetch origin
git rebase master
```

You may be asked to perform actions like squashing, rewording, or editing commits. See the [Rewriting history in Git](https://www.youtube.com/watch?v=ElRzTuYln0M) YouTube tutorial for more information.

### Creating a pull request
Make sure your code meets the requirements in the [full contribution guidelines](/CONTRIBUTING.md) and [coding style](CodingStyle.md). Additionally:

* Make correctly formatted, atomic commits (building the project at every commit should succeed).
* Discuss and resolve any reviews you receive.
* Fix CI failures by editing your commits.
