# Ladybird TODO Analysis & Contribution Strategy

**Date**: 2025-10-24
**Status**: Initial analysis after dev environment setup

## TODO Distribution Overview

**Total TODOs/FIXMEs**: ~3,584 items across the codebase

### Breakdown by Type
- **Comment TODOs** (`// TODO:`): ~99 items (planning/future work)
- **Runtime TODOs** (`TODO()`): ~65 items (unimplemented code paths that will crash)
- **FIXMEs**: ~3,420 items (known issues, workarounds, improvements)

### Distribution by Area
| Component | Files with TODOs | Priority Level |
|-----------|------------------|----------------|
| Libraries | 653 files | **HIGH** (core functionality) |
| LibWeb | 450+ files | **CRITICAL** (web engine) |
| AK | 45 files | **MEDIUM** (foundational) |
| Tests | 24 files | **LOW** (test infrastructure) |
| Services | 19 files | **HIGH** (process architecture) |
| Meta | 13 files | **LOW** (build tooling) |
| UI | 12 files | **MEDIUM** (user interface) |

## TODO Categories by Complexity

### 1. **Quick Wins** (Beginner-Friendly)
**Characteristics**: Single file, clear scope, minimal dependencies
- Missing error messages or logging
- Simple parameter validation
- Documentation improvements
- Code style consistency fixes
- Adding missing test cases

**Example areas**:
- `AK/` utility improvements (string handling, container optimizations)
- `LibCore` platform-specific implementations
- Test coverage gaps

### 2. **Feature Completions** (Intermediate)
**Characteristics**: Well-defined scope, existing pattern to follow
- Implementing missing CSS properties
- Adding missing DOM API methods
- Completing partially implemented web standards
- Platform-specific feature parity

**Example areas**:
- CSS property implementations (many marked with spec references)
- DOM/HTML element methods (check MDN for spec compliance)
- JavaScript built-in objects/methods

### 3. **Architecture Improvements** (Advanced)
**Characteristics**: Multi-file, requires deep understanding
- Performance optimizations
- Security enhancements
- Process architecture improvements
- Memory management refinements

**Example areas**:
- Multi-process communication efficiency
- Rendering pipeline optimizations
- Memory leak fixes

### 4. **Research Required** (Expert)
**Characteristics**: Unclear solution, may need design discussion
- Complex spec interpretation
- Novel algorithm implementations
- Cross-platform compatibility challenges
- Security model design

## Recommended Contribution Path

### **Phase 1: Foundation (Weeks 1-2)**
**Goal**: Build familiarity without committing code yet

1. **Build & run the browser**
   ```bash
   ./Meta/ladybird.py run
   ```

2. **Browse real websites** - Note what works vs breaks
   - Simple static sites (work well)
   - Complex web apps (many TODOs will be obvious)

3. **Read architecture docs**
   - `Documentation/ProcessArchitecture.md`
   - `Documentation/LibWebFromLoadingToPainting.md`
   - `Documentation/LibWebPatterns.md`

4. **Study the test suite**
   ```bash
   ./Meta/ladybird.py test LibWeb
   ```

5. **Join Discord** - https://discord.gg/nvfjVJ4Svh
   - Ask maintainers about good first issues
   - Learn current priorities

### **Phase 2: First Contribution (Week 3)**
**Goal**: Get familiar with contribution workflow

**Recommended starting points**:

1. **Add missing test coverage**
   - Find untested code paths in LibWeb
   - Use `./Tests/LibWeb/add_libweb_test.py`
   - Low risk, high learning value

2. **Fix documentation TODOs**
   - Improve code comments
   - Add missing documentation
   - Update outdated info

3. **Tackle "good first issue" on GitHub**
   - Check issue tracker for tagged issues
   - Ask on Discord for recommendations

### **Phase 3: Feature Work (Weeks 4+)**
**Goal**: Meaningful contributions to web standards support

**High-Value Areas**:

1. **CSS Properties**
   - Many properties partially implemented
   - Follow `Documentation/CSSProperties.md`
   - Test against Web Platform Tests

2. **DOM/HTML APIs**
   - Check MDN for missing methods
   - Implement following LibWeb patterns
   - Add comprehensive tests

3. **JavaScript Built-ins**
   - LibJS has many TODO methods
   - Follow ECMAScript spec
   - Test with Test262 suite

### **Phase 4: Advanced Features (Month 2+)**
**Goal**: Add novel features or significant improvements

**Potential Feature Ideas**:

1. **Developer Tools Enhancements**
   - Network inspector improvements
   - Performance profiling tools
   - Memory inspection UI

2. **Accessibility Features**
   - Screen reader support improvements
   - Keyboard navigation enhancements
   - ARIA implementation completeness

3. **Performance Optimizations**
   - Rendering pipeline improvements
   - JavaScript engine optimizations
   - Memory usage reductions

4. **Web Standards Compliance**
   - Pick a Web Platform Test suite
   - Work through failures systematically
   - Implement missing features

## Strategic TODO Selection Guide

### For Learning the Codebase
**Start with**: AK utilities, LibCore platform code
- Self-contained
- Well-documented patterns
- Clear success criteria

### For Web Standards Work
**Start with**: CSS properties, DOM methods
- Specs are authoritative reference
- Existing patterns to follow
- Web Platform Tests validate correctness

### For JavaScript Work
**Start with**: Built-in object methods
- ECMAScript spec is reference
- Test262 test suite available
- Clear expected behavior

### For Performance Work
**Start with**: Profiling existing code first
- Measure before optimizing
- Use sanitizer builds to catch issues
- Benchmark improvements

## Tools for TODO Hunting

### Find TODOs by type
```bash
# Runtime TODOs (will crash if executed)
grep -r "TODO()" --include="*.cpp" --include="*.h" Libraries/

# Comment TODOs (future work)
grep -r "// TODO:" --include="*.cpp" --include="*.h" Libraries/

# FIXMEs (known issues)
grep -r "FIXME:" --include="*.cpp" --include="*.h" Libraries/
```

### Find TODOs in specific area
```bash
# CSS-related
grep -r "TODO\|FIXME" Libraries/LibWeb/CSS/

# JavaScript-related
grep -r "TODO\|FIXME" Libraries/LibJS/

# DOM-related
grep -r "TODO\|FIXME" Libraries/LibWeb/DOM/
```

### Find TODOs with context
```bash
# Show 2 lines before and after
grep -r -B2 -A2 "TODO:" Libraries/LibWeb/ | less
```

## Important Guidelines

### Before Starting Any TODO

1. **Check GitHub issues** - May already be tracked or in progress
2. **Ask on Discord** - Maintainers may have context
3. **Read related docs** - Understand the component first
4. **Check git history** - See why TODO was added
5. **Review tests** - Understand expected behavior

### Quality Standards

- **Always add tests** for bug fixes and features
- **Follow existing patterns** - Don't introduce new styles
- **Use C++23 features** - Not C-style code
- **Follow commit format** - `Category: Description` (no period)
- **Run clang-format** - `ninja -C Build/release check-style`
- **Run tests** - `./Meta/ladybird.py test`

### Red Flags (Avoid These TODOs Initially)

- TODOs with "FIXME: This is wrong" (needs design discussion)
- TODOs in performance-critical paths (needs profiling first)
- TODOs mentioning "once we have X" (waiting on other work)
- TODOs with multiple question marks (unclear solution)
- Architecture TODOs (needs deep familiarity)

## Next Steps for You

Based on your current state (dev environment ready):

1. **Immediate (Today)**
   - Build and run: `./Meta/ladybird.py run`
   - Browse to a few websites, note what works
   - Join Discord and introduce yourself

2. **This Week**
   - Read 3 key docs: ProcessArchitecture, LibWebPatterns, LibWebFromLoadingToPainting
   - Run test suite: `./Meta/ladybird.py test`
   - Find 3 "good first issue" items on GitHub

3. **Next Week**
   - Pick one small TODO (test coverage or documentation)
   - Make your first PR
   - Get feedback, iterate

4. **Month 1**
   - Complete 2-3 small contributions
   - Identify an area you want to specialize in
   - Start planning your first feature addition

## Feature Addition Ideas (After Foundation)

Once you're comfortable with the codebase:

### **Developer Experience Improvements**
- Enhanced DevTools inspector
- Better error messages
- Performance profiling UI
- Memory leak detection tools

### **Web Standards Compliance**
- Pick a WPT test suite with low pass rate
- Systematically implement missing features
- Document your progress

### **User-Facing Features**
- Reader mode
- Dark mode support
- Print preview
- Download manager improvements

### **Platform-Specific Features**
- Windows-specific optimizations
- Better Windows UI integration
- Platform-specific shortcuts

## Resources

- **Discord**: https://discord.gg/nvfjVJ4Svh
- **GitHub Issues**: https://github.com/LadybirdBrowser/ladybird/issues
- **Good First Issues**: Filter by "good first issue" label
- **Documentation**: `Documentation/` directory
- **Web Standards**: https://html.spec.whatwg.org/, https://w3.org/Style/CSS/
- **Test Suites**: Web Platform Tests (WPT), Test262 (JavaScript)

---

**Remember**: Quality over quantity. One solid, well-tested contribution is worth more than 10 half-baked changes. Take time to understand the codebase deeply before attempting major features.
