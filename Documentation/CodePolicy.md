# Code Policy

This document describes expectations for code maintained in upstream Ladybird. It is primarily intended for project maintainers and people studying local Ladybird development.

## Testing policy

When possible, code changes should include tests when fixing bugs or adding new features.

If changes have relevant [Web Platform Tests (WPT)](https://wpt.fyi/) tests - especially if the changes cause Ladybird to pass any WPT tests it had not yet been passing - consider [importing those tests into the Ladybird tree](Testing.md#importing-web-platform-tests), and then committing the imported tests along with the code changes.

## Human language policy

In Ladybird, we treat human language as seriously as we do programming language.

The following applies to all user-facing strings, code, comments, and commit messages:

* The official project language is American English with ISO 8601 dates and metric units.
* Use proper spelling, grammar, and punctuation.
* Write in an authoritative and technical tone.
* Avoid contractions, slang, and idioms.
* Avoid humor, sarcasm, and other forms of non-literal language.
* Use gender-neutral pronouns, except when referring to a specific person.

Note that this also applies to debug logging and other internal strings, as they may be exposed to users in the future.

## Code policy

Nobody is perfect, and sometimes we mess things up. That said, here are some good do's & don'ts to try and stick to:

**Do:**

* Write in idiomatic project-style C++23, using the `AK` containers in all code.
* Conform to the project coding style found in [CodingStyle.md](CodingStyle.md). Use `clang-format` to automatically format C++ files.
* Choose expressive variable, function and class names. Make it as obvious as possible what the code is doing.
* Split your changes into separate, atomic commits (i.e. A commit per feature or fix, where the build, tests and the system are all functioning).
* Make sure your commits are rebased on the master branch.
* Wrap your commit messages at 72 characters.
* The first line of the commit message is the subject line, and must have the format "Category: Brief description of what's being changed". The category should be the name of a library, application, service, utility, etc.
  * Examples: `LibMedia`, `WebContent`, `CI`, `AK`, `RequestServer`, `js`
  * Don't use a category like "`Libraries`" or "`Utilities`", except for generic changes that affect a large portion of code within these directories.
  * Don't use specific component names, e.g. C++ class names, as the category either - mention them in the summary instead. E.g. `LibGUI: Brief description of what's being changed in FooWidget` rather than `FooWidget: Brief description of what's being changed`
  * Several categories may be combined with `+`, e.g. `LibJS+LibWeb+Browser: ...`
* Write the commit message subject line in the imperative mood ("Foo: Change the way dates work", not "Foo: Changed the way dates work").
* Write your commit messages in proper English, with care and punctuation.
* Amend existing commits when adding changes after a review, where relevant.
* Mark each review comment as "resolved" after pushing a fix with the requested changes, where relevant.
* Add your personal copyright line to files when making substantive changes. (Optional but encouraged!)
* Check the spelling of your code, comments and commit messages.
* If you have images that go along with your code, run `optipng -strip all` on them to optimize and strip away useless metadata - this can reduce file size from multiple kilobytes to a couple hundred bytes.

**Don't:**

* Introduce changes that are incompatible with the project licence (2-clause BSD.)
* Touch anything outside the stated scope of the change.
* Iterate excessively on your design across multiple commits.
* Use weasel-words like "refactor" or "fix" to avoid explaining what's being changed.
* End commit message subject lines with a period.
* Include commented-out code.
* Write in C. (Instead, take advantage of C++'s amenities, and don't limit yourself to the standard C library.)
* Attempt large architectural changes until you are familiar with the system and have worked on it for a while.
* Engage in excessive "feng shui programming" by moving code around without quantifiable benefit.
* Add jokes or other "funny" things to user-facing parts of the system.

## On usage of AI and LLMs

Usage of AI assistance is usually fine, but you are responsible for making sure the quality of the output is up to the standards of the project. Currently, AI-generated output is often too verbose and its quality is subpar compared to carefully written human work.

Do not use an AI or LLM to generate changes, respond to issues, or participate in project discussion without holding the output to the same standards as human-written content.

Do not use AI-generated descriptions or summaries as a substitute for understanding the relevant code, issue, or discussion.

## Commit hooks

The repository contains a file called `.pre-commit-config.yaml` that defines several commit hooks that can be run automatically just before and after creating a new commit. These hooks lint your commit message, and the changes it contains to ensure they will pass the automated CI checks.

To enable these hooks, first follow the installation instructions available at https://pre-commit.com/#install and then enable one or both of the following hooks:

* pre-commit hook - Runs `Meta/lint-ci.sh` and `Meta/lint-ports.py` to ensure changes to the code will pass linting:

  ```console
  pre-commit install
  ```

* post-commit hook - Lints the commit message to ensure it will pass the commit linting:

  ```console
  pre-commit install --hook-type commit-msg
  ```

## Git notes

The GitHub project contains [git notes](https://git-scm.com/docs/git-notes) for each commit that includes e.g. a link to the pull request from which the commit originated and reviewer information. These are updated automatically, but require an additional step locally to be able to see the notes in `git log`:

```bash
git config --add remote.upstream.fetch '+refs/notes/*:refs/notes/*'
```

> [!NOTE]
> The `upstream` remote in this command should be replaced with whatever you've named the LadybirdBrowser/ladybird.git
> remote in your local clone. Use `git remote -v` to find that name.

Now, any time you `git fetch`, the latest notes will be fetched as well. You will see information like the following when you run `git log`:

```
commit c1b0e180ba64d2ea7e815e2c2e93087ae9a26500
Author: Timothy Flynn <trflynn89@pm.me>
Date:   Mon Jul 29 10:18:25 2024 -0400

    LibWebView: Insert line numbers before each line in about:srcdoc

    The behavior chosen here (fixed-width counters, alignment, etc.) matches
    Firefox.

Notes:
    Author: https://github.com/trflynn89
    Commit: https://github.com/LadybirdBrowser/ladybird/commit/c1b0e180ba6
    Pull-request: https://github.com/LadybirdBrowser/ladybird/pull/892
    Reviewed-by: https://github.com/AtkinsSJ ✅
```
