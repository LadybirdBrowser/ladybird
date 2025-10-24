#!/usr/bin/env bash
# String Safety Check Script for Ladybird Browser
# Prevents introduction of unsafe C string functions
#
# Usage:
#   ./Meta/check-string-safety.sh          # Check entire codebase
#   ./Meta/check-string-safety.sh --strict # Exit on any finding

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Unsafe C string functions to detect
UNSAFE_FUNCTIONS=(
    "strcpy"
    "strcat"
    "sprintf"
    "gets"
    "strncpy"
    "strncat"
)

# Directories to check
CHECK_DIRS=(
    "Libraries"
    "Services"
    "UI"
    "AK"
)

STRICT_MODE=false
if [[ "${1:-}" == "--strict" ]]; then
    STRICT_MODE=true
fi

echo "🔍 Checking for unsafe C string functions in Ladybird codebase..."
echo ""

TOTAL_VIOLATIONS=0
VIOLATIONS_BY_FUNCTION=()

for func in "${UNSAFE_FUNCTIONS[@]}"; do
    # Search for actual function calls (not just comments or string literals)
    # Pattern: word boundary + function name + optional whitespace + opening paren
    PATTERN="\\b${func}\\s*\\("

    FOUND_FILES=()
    for dir in "${CHECK_DIRS[@]}"; do
        if [[ ! -d "$PROJECT_ROOT/$dir" ]]; then
            continue
        fi

        # Use grep to find matches
        # -r: recursive
        # -n: show line numbers
        # -H: show filenames
        # --include: only check C/C++ files
        # -E: extended regex
        while IFS= read -r match; do
            # Extract filename and line number
            file=$(echo "$match" | cut -d: -f1)
            line=$(echo "$match" | cut -d: -f2)
            code=$(echo "$match" | cut -d: -f3-)

            # Skip false positives in comments
            if echo "$code" | grep -q "^[[:space:]]*//"; then
                continue
            fi

            # Skip false positives in string literals (simple heuristic)
            if echo "$code" | grep -q "\".*${func}.*\""; then
                # Check if it's inside a string literal
                # This is a simple check - may have false negatives
                continue
            fi

            # This is a real violation
            FOUND_FILES+=("$file:$line: $code")
            ((TOTAL_VIOLATIONS++))
        done < <(grep -rn -E "$PATTERN" --include="*.cpp" --include="*.h" "$PROJECT_ROOT/$dir" 2>/dev/null || true)
    done

    if [[ ${#FOUND_FILES[@]} -gt 0 ]]; then
        echo -e "${RED}❌ Found ${#FOUND_FILES[@]} instances of unsafe function: ${func}()${NC}"
        VIOLATIONS_BY_FUNCTION+=("$func:${#FOUND_FILES[@]}")

        # Show first 10 instances
        for i in "${!FOUND_FILES[@]}"; do
            if [[ $i -ge 10 ]]; then
                echo "   ... and $((${#FOUND_FILES[@]} - 10)) more"
                break
            fi
            echo "   ${FOUND_FILES[$i]}"
        done
        echo ""
    fi
done

# Summary
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [[ $TOTAL_VIOLATIONS -eq 0 ]]; then
    echo -e "${GREEN}✅ SUCCESS: No unsafe C string functions found!${NC}"
    echo ""
    echo "Ladybird codebase is using safe AK string classes exclusively."
    echo ""
    echo "Safe alternatives:"
    echo "  strcpy()  → String::from_utf8() or String::copy()"
    echo "  strcat()  → String::formatted() or StringBuilder"
    echo "  sprintf() → String::formatted() with compile-time checking"
    echo "  gets()    → NEVER USE (use String::from_stream())"
    echo ""
    exit 0
else
    echo -e "${RED}❌ FAILURE: Found $TOTAL_VIOLATIONS unsafe C string function calls${NC}"
    echo ""
    echo "Violations by function:"
    for violation in "${VIOLATIONS_BY_FUNCTION[@]}"; do
        func=$(echo "$violation" | cut -d: -f1)
        count=$(echo "$violation" | cut -d: -f2)
        echo "  $func(): $count instances"
    done
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""
    echo "How to fix:"
    echo ""
    echo "1. Replace strcpy() with safe alternatives:"
    echo "   char dst[256];"
    echo "   strcpy(dst, src);  ❌ UNSAFE"
    echo "   →"
    echo "   auto dst = String::from_utf8(src);  ✅ SAFE"
    echo ""
    echo "2. Replace strcat() with StringBuilder or formatted():"
    echo "   strcat(dst, src);  ❌ UNSAFE"
    echo "   →"
    echo "   auto result = String::formatted(\"{}{}\", dst, src);  ✅ SAFE"
    echo ""
    echo "3. Replace sprintf() with String::formatted():"
    echo "   sprintf(buf, \"Value: %d\", x);  ❌ UNSAFE"
    echo "   →"
    echo "   auto buf = String::formatted(\"Value: {}\", x);  ✅ SAFE"
    echo ""
    echo "4. Never use gets() - always unsafe:"
    echo "   gets(buffer);  ❌ ALWAYS UNSAFE"
    echo "   →"
    echo "   auto input = String::from_stream(stdin, max_bytes);  ✅ SAFE"
    echo ""
    echo "See Documentation/StringSafety.md for complete migration guide."
    echo ""

    if [[ "$STRICT_MODE" == "true" ]]; then
        exit 1
    else
        echo -e "${YELLOW}⚠️  WARNING: Unsafe functions found but --strict not specified${NC}"
        echo "   Add --strict flag to fail CI/CD on violations"
        exit 0
    fi
fi
