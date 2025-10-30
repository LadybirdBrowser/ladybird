# Sentinel YARA Rules Guide

**Version**: 0.1.0 (MVP)
**Last Updated**: 2025-10-29
**Audience**: Advanced Users and Security Researchers

---

## Table of Contents

1. [Introduction to YARA](#introduction-to-yara)
2. [YARA Basics](#yara-basics)
3. [Setting Up Custom Rules](#setting-up-custom-rules)
4. [Example Rules](#example-rules)
5. [Rule Performance Optimization](#rule-performance-optimization)
6. [Testing Your Rules](#testing-your-rules)
7. [Community Rules](#community-rules)
8. [Troubleshooting](#troubleshooting)
9. [Best Practices](#best-practices)
10. [Advanced Techniques](#advanced-techniques)

---

## Introduction to YARA

### What is YARA?

YARA (Yet Another Ridiculous Acronym) is a pattern-matching tool designed for malware researchers to identify and classify malware samples. Think of it as a "virus scanner for malware researchers" - it lets you write rules that describe malware families or suspicious patterns.

**Key Features**:
- Pattern-based detection using strings and conditions
- Fast scanning of files and memory
- Boolean logic for complex detection scenarios
- Module system for extended functionality
- Industry-standard tool used by security professionals worldwide

### Why YARA for Sentinel?

Sentinel uses YARA as its core detection engine because:

1. **Flexibility**: Write custom rules for any threat type
2. **Performance**: Highly optimized C library
3. **Community**: Large library of existing rules
4. **Extensibility**: Module system for advanced features
5. **Standard**: Well-documented and widely understood

### YARA in the Sentinel Workflow

```
Download starts
  ↓
SecurityTap extracts file content
  ↓
SentinelServer receives content
  ↓
YARA scans content against all loaded rules
  ↓
If match: Generate security alert
  ↓
PolicyGraph checks for existing policy
  ↓
Enforce action or show user dialog
```

---

## YARA Basics

### Rule Structure

Every YARA rule follows this basic structure:

```yara
rule RuleName {
    meta:
        // Metadata about the rule
        description = "What this rule detects"
        author = "Your Name"
        date = "2025-10-29"
        severity = "high"

    strings:
        // Patterns to search for
        $string1 = "suspicious pattern"
        $string2 = { 4D 5A 90 00 }  // Hex pattern
        $regex1 = /malware[0-9]{3}/ nocase

    condition:
        // Logic for when rule matches
        any of them
}
```

### Components Explained

#### 1. Rule Name

```yara
rule Win32_Trojan_Generic {
    ...
}
```

**Requirements**:
- Must start with letter or underscore
- Can contain letters, digits, underscores
- No spaces or special characters
- Should be descriptive and unique

**Naming Conventions**:
```
Platform_Type_Variant_Version

Examples:
- Win32_Trojan_Emotet_v3
- JS_Obfuscator_Generic
- PDF_Exploit_CVE_2023_12345
- Android_Malware_Triada
```

---

#### 2. Meta Section

Metadata provides context about the rule (not used in matching):

```yara
meta:
    description = "Detects Emotet trojan variant"
    author = "Security Team"
    date = "2025-10-29"
    reference = "https://malpedia.caad.fkie.fraunhofer.de/details/win.emotet"
    severity = "critical"  // critical, high, medium, low
    tlp = "white"  // Traffic Light Protocol
    version = "1.0"
```

**Sentinel-Specific Metadata**:
```yara
meta:
    sentinel_action = "block"  // Recommended action
    sentinel_confidence = "high"  // How confident is this detection
    false_positive_risk = "low"  // Likelihood of false positives
```

---

#### 3. Strings Section

Define patterns to search for in files:

**Text Strings**:
```yara
strings:
    $text1 = "malicious string"  // Case-sensitive
    $text2 = "suspicious" nocase  // Case-insensitive
    $text3 = "wide string" wide  // UTF-16 encoded
    $text4 = "full word" fullword  // Must be whole word
```

**Hex Patterns**:
```yara
strings:
    // MZ header (PE file signature)
    $mz_header = { 4D 5A }

    // With wildcards (? = any nibble, ?? = any byte)
    $pattern = { 4D 5A ?? ?? ?? ?? 50 45 }

    // With jumps (skip bytes)
    $jump_pattern = { 4D 5A [0-8] 50 45 }  // Skip 0-8 bytes

    // Alternatives
    $alternatives = { 4D 5A | 5A 4D }  // Match either
```

**Regular Expressions**:
```yara
strings:
    $regex1 = /malware[0-9]{3}/  // Match "malware" followed by 3 digits
    $regex2 = /http:\/\/[a-z]+\.ru/ nocase  // Match .ru URLs
    $regex3 = /\b(?:\d{1,3}\.){3}\d{1,3}\b/  // Match IP addresses
```

**String Modifiers**:
```yara
strings:
    $s1 = "test" nocase  // Case-insensitive
    $s2 = "test" wide  // UTF-16LE encoding
    $s3 = "test" ascii wide  // Match both ASCII and wide
    $s4 = "test" fullword  // Must have non-alphanumeric boundaries
    $s5 = "test" xor  // Match XOR-encoded (all single-byte keys)
    $s6 = "test" base64  // Match base64-encoded
```

---

#### 4. Condition Section

Boolean logic that determines when the rule matches:

**Basic Conditions**:
```yara
condition:
    any of them  // Match if ANY string is found
    all of them  // Match if ALL strings are found
    $string1  // Match if specific string is found
    $string1 and $string2  // Logical AND
    $string1 or $string2  // Logical OR
    not $string1  // Logical NOT
```

**String Counts**:
```yara
condition:
    #string1 > 5  // String appears more than 5 times
    #string1 == 1  // String appears exactly once
    2 of ($string1, $string2, $string3)  // Exactly 2 of these strings
```

**String Sets**:
```yara
strings:
    $s1 = "pattern1"
    $s2 = "pattern2"
    $s3 = "pattern3"

condition:
    any of ($s*)  // Any string starting with $s
    2 of ($s1, $s2, $s3)  // At least 2 of these 3 strings
```

**File Size**:
```yara
condition:
    filesize < 100KB  // File smaller than 100KB
    filesize > 1MB and filesize < 10MB
```

**String Locations**:
```yara
condition:
    $mz_header at 0  // MZ header at file offset 0
    $string1 in (0..1024)  // String in first 1KB
    $string2 in (filesize-1024..filesize)  // String in last 1KB
```

**Complex Conditions**:
```yara
condition:
    // PE file with suspicious imports
    uint16(0) == 0x5A4D and  // MZ header
    filesize < 2MB and
    (
        (#import_LoadLibrary > 10) or
        ($suspicious_api and #network_call > 3)
    )
```

---

### Complete Example Rule

```yara
rule Win32_Trojan_Emotet_Downloader {
    meta:
        description = "Detects Emotet trojan downloader variant"
        author = "Sentinel Security Team"
        date = "2025-10-29"
        reference = "https://malpedia.caad.fkie.fraunhofer.de/details/win.emotet"
        severity = "critical"
        sentinel_action = "block"
        false_positive_risk = "low"

    strings:
        // PE header
        $mz_header = { 4D 5A }
        $pe_header = { 50 45 00 00 }

        // Emotet-specific strings
        $cmd1 = "cmd.exe /c" nocase
        $cmd2 = "powershell.exe -enc" nocase
        $regkey = "Software\\Microsoft\\Windows\\CurrentVersion\\Run" nocase

        // Network patterns
        $url_pattern = /https?:\/\/[a-z0-9]+\.[a-z]{2,6}\/[a-z0-9]+/ nocase

        // Suspicious API calls
        $api1 = "CreateRemoteThread" ascii
        $api2 = "WriteProcessMemory" ascii
        $api3 = "VirtualAllocEx" ascii

        // Encoding/obfuscation
        $base64_chunk = /[A-Za-z0-9+\/]{50,}==?/ ascii

    condition:
        // Must be PE file
        $mz_header at 0 and
        $pe_header and

        // Reasonable file size for malware
        filesize > 20KB and filesize < 5MB and

        // Detection logic
        (
            // Command execution indicators
            (2 of ($cmd*) and $regkey) or

            // Process injection indicators
            (all of ($api*)) or

            // Network + obfuscation
            ($url_pattern and $base64_chunk and #cmd1 > 2)
        )
}
```

---

## Setting Up Custom Rules

### Rule Directory

Sentinel loads YARA rules from:

**Linux/macOS**:
```
~/.local/share/Ladybird/sentinel/rules/
```

**Windows**:
```
%LOCALAPPDATA%\Ladybird\sentinel\rules\
```

### Adding a New Rule

1. **Create rule file**:
   ```bash
   cd ~/.local/share/Ladybird/sentinel/rules/
   nano my_custom_rule.yar
   ```

2. **Write your rule**:
   ```yara
   rule My_Custom_Threat {
       meta:
           description = "Detects my custom threat"
           author = "My Name"
           date = "2025-10-29"
           severity = "high"

       strings:
           $pattern = "malicious_pattern"

       condition:
           $pattern
   }
   ```

3. **Save and reload**:
   ```bash
   # Restart SentinelServer to load new rules
   pkill SentinelServer
   /path/to/Build/release/bin/SentinelServer &
   ```

4. **Verify rule loaded**:
   ```bash
   # Check SentinelServer logs
   tail -f ~/.local/share/Ladybird/sentinel.log

   # Should see: "Loaded rule: My_Custom_Threat"
   ```

### Rule File Organization

Organize rules by category:

```
~/.local/share/Ladybird/sentinel/rules/
├── malware/
│   ├── win32_trojans.yar
│   ├── ransomware.yar
│   └── android_malware.yar
├── exploits/
│   ├── pdf_exploits.yar
│   └── office_exploits.yar
├── suspicious/
│   ├── obfuscation.yar
│   ├── packers.yar
│   └── scripts.yar
├── false_positives/
│   └── security_tools.yar  (use with Allow policies)
└── custom/
    └── my_rules.yar
```

**Index file** (optional):
```yara
// index.yar - Include all rule files
include "malware/win32_trojans.yar"
include "malware/ransomware.yar"
include "exploits/pdf_exploits.yar"
include "suspicious/obfuscation.yar"
include "custom/my_rules.yar"
```

---

## Example Rules

### Example 1: EICAR Test File

The EICAR test file is a safe way to test malware detection:

```yara
rule Test_File_EICAR {
    meta:
        description = "Detects EICAR anti-virus test file"
        author = "Sentinel Team"
        reference = "https://www.eicar.org/"
        severity = "low"
        is_test = "true"

    strings:
        // EICAR test string
        $eicar = "X5O!P%@AP[4\\PZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*"

    condition:
        $eicar
}
```

**Use**: Test if Sentinel is working without real malware.

---

### Example 2: Windows PE Malware Detector

Detect suspicious Windows executables:

```yara
import "pe"

rule Win32_Suspicious_PE {
    meta:
        description = "Detects suspicious Windows PE files"
        author = "Sentinel Team"
        date = "2025-10-29"
        severity = "medium"

    strings:
        // Suspicious imports
        $import1 = "CreateRemoteThread" ascii
        $import2 = "WriteProcessMemory" ascii
        $import3 = "VirtualAllocEx" ascii
        $import4 = "SetWindowsHookEx" ascii

        // Registry persistence
        $reg1 = "Software\\Microsoft\\Windows\\CurrentVersion\\Run" nocase
        $reg2 = "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce" nocase

        // Anti-debugging
        $antidebug1 = "IsDebuggerPresent" ascii
        $antidebug2 = "CheckRemoteDebuggerPresent" ascii

    condition:
        // Must be PE file
        uint16(0) == 0x5A4D and

        // Reasonable size
        filesize > 10KB and filesize < 10MB and

        // Suspicious indicators
        (
            // Process injection
            (3 of ($import*)) or

            // Persistence + anti-debugging
            (any of ($reg*) and any of ($antidebug*))
        ) and

        // PE-specific checks using pe module
        pe.number_of_sections > 2 and
        pe.number_of_sections < 10
}
```

---

### Example 3: JavaScript Obfuscation Detector

Detect obfuscated JavaScript code:

```yara
rule JS_Obfuscated_Code {
    meta:
        description = "Detects heavily obfuscated JavaScript"
        author = "Sentinel Team"
        severity = "medium"
        false_positive_risk = "medium"  // Minified JS may trigger

    strings:
        // Common obfuscation patterns
        $pattern1 = /eval\s*\(\s*.*\s*\)/ nocase
        $pattern2 = /String\.fromCharCode/ nocase
        $pattern3 = /unescape\s*\(/ nocase
        $pattern4 = /document\.write\s*\(\s*unescape/ nocase

        // Base64 encoded strings (long)
        $base64 = /[A-Za-z0-9+\/]{100,}={0,2}/ ascii

        // Hex encoded strings
        $hex_pattern = /\\x[0-9a-fA-F]{2}/ ascii

        // Unicode escape sequences
        $unicode = /\\u[0-9a-fA-F]{4}/ ascii

    condition:
        // High density of obfuscation
        (
            (#pattern1 > 5) or
            (#pattern2 > 10) or
            ($pattern3 and $pattern4)
        ) and
        (
            (#base64 > 3) or
            (#hex_pattern > 50) or
            (#unicode > 50)
        )
}
```

---

### Example 4: PDF with Embedded Executable

Detect PDFs with embedded Windows executables:

```yara
rule PDF_Embedded_EXE {
    meta:
        description = "Detects PDF files with embedded Windows executables"
        author = "Sentinel Team"
        severity = "high"
        reference = "https://blog.malwarebytes.com/threat-analysis/"

    strings:
        // PDF header
        $pdf_header = "%PDF-" ascii

        // PE executable signatures (MZ header)
        $mz_header = { 4D 5A }
        $pe_header = { 50 45 00 00 }

        // PDF object with embedded file
        $embeddedfile = "/EmbeddedFile" ascii
        $type_objstm = "/Type /ObjStm" ascii

        // JavaScript in PDF (used to launch)
        $js_launch = "/JS" ascii
        $js_action = "/JavaScript" ascii
        $openaction = "/OpenAction" ascii

    condition:
        // Must be PDF
        $pdf_header at 0 and

        // Contains PE executable
        $mz_header and $pe_header and

        // Embedded file structure
        $embeddedfile and

        // Auto-launch mechanism
        (
            ($js_launch and $js_action) or
            $openaction
        )
}
```

---

### Example 5: Office Document with Macros

Detect Office documents with suspicious macros:

```yara
rule Office_Suspicious_Macro {
    meta:
        description = "Detects Office documents with suspicious VBA macros"
        author = "Sentinel Team"
        severity = "high"
        false_positive_risk = "low"

    strings:
        // Office file signatures
        $office_sig = { D0 CF 11 E0 A1 B1 1A E1 }  // OLE compound file
        $ooxml_sig = { 50 4B 03 04 }  // ZIP (modern Office)

        // VBA macro indicators
        $vba1 = "VBA" ascii
        $vba2 = "_VBA_PROJECT" wide

        // Auto-execution functions
        $auto1 = "AutoOpen" ascii nocase
        $auto2 = "Auto_Open" ascii nocase
        $auto3 = "Workbook_Open" ascii nocase
        $auto4 = "Document_Open" ascii nocase

        // Suspicious actions
        $cmd = "cmd.exe" ascii nocase
        $powershell = "powershell" ascii nocase
        $wscript = "wscript.shell" ascii nocase
        $download = "URLDownloadToFile" ascii nocase

        // Obfuscation in macros
        $chr = "Chr(" ascii nocase
        $strreverse = "StrReverse" ascii nocase

    condition:
        // Must be Office file
        ($office_sig at 0 or $ooxml_sig at 0) and

        // Contains VBA
        ($vba1 or $vba2) and

        // Auto-execution
        any of ($auto*) and

        // Suspicious behavior
        (
            any of ($cmd, $powershell, $wscript, $download) or
            (#chr > 10 and $strreverse)
        )
}
```

---

### Example 6: Archive Bomb Detector

Detect compression bombs (zip bombs):

```yara
rule Archive_Bomb {
    meta:
        description = "Detects potential zip bombs and decompression attacks"
        author = "Sentinel Team"
        severity = "medium"

    strings:
        // ZIP file signature
        $zip_sig = { 50 4B 03 04 }

        // Repeated patterns (highly compressible data)
        $repeated_pattern = { 00 00 00 00 00 00 00 00 00 00 }

    condition:
        $zip_sig at 0 and

        // Small file size but lots of repeated patterns
        filesize < 1MB and
        #repeated_pattern > 100 and

        // High compression ratio indicator
        // (This is simplified; real detection would check ZIP metadata)
        filesize < 50KB
}
```

---

### Example 7: Ransomware Indicator

Detect common ransomware patterns:

```yara
rule Win32_Ransomware_Generic {
    meta:
        description = "Detects common ransomware indicators"
        author = "Sentinel Team"
        severity = "critical"

    strings:
        // Ransom note patterns
        $ransom1 = "Your files have been encrypted" nocase
        $ransom2 = "pay" and "bitcoin" nocase
        $ransom3 = "decrypt" and "private key" nocase

        // File extension changes
        $ext1 = ".encrypted" nocase
        $ext2 = ".locked" nocase
        $ext3 = ".crypto" nocase

        // Crypto APIs (used for file encryption)
        $crypto1 = "CryptEncrypt" ascii
        $crypto2 = "CryptGenKey" ascii
        $crypto3 = "CryptAcquireContext" ascii

        // File operations
        $file_op1 = "FindFirstFile" ascii
        $file_op2 = "FindNextFile" ascii

        // Network (for key exchange)
        $net1 = "InternetOpen" ascii
        $net2 = "HttpSendRequest" ascii

        // Process termination (kill security software)
        $kill = "TerminateProcess" ascii

    condition:
        // Must be PE file
        uint16(0) == 0x5A4D and

        // Ransomware indicators
        (
            // Ransom note content
            (2 of ($ransom*)) or

            // Crypto + file operations + network
            (
                (2 of ($crypto*)) and
                (all of ($file_op*)) and
                (any of ($net*))
            ) or

            // Extension changing + encryption
            (any of ($ext*) and any of ($crypto*))
        ) and

        // Anti-security
        $kill
}
```

---

## Rule Performance Optimization

### Performance Considerations

YARA rules can impact scan speed. Follow these guidelines:

### 1. Use Specific Strings

**Slow**:
```yara
strings:
    $s = "a"  // Too common, will match millions of times
condition:
    $s
```

**Fast**:
```yara
strings:
    $s = "specific_malware_string_12345"  // Unique, rare matches
condition:
    $s
```

**Guideline**: Strings should be at least 4-8 characters for reasonable performance.

---

### 2. Limit Regex Complexity

**Slow**:
```yara
strings:
    // Catastrophic backtracking
    $regex = /(a+)+b/
condition:
    $regex
```

**Fast**:
```yara
strings:
    // Bounded quantifiers
    $regex = /a{1,50}b/
condition:
    $regex
```

**Guidelines**:
- Avoid nested quantifiers: `(a+)+`, `(a*)*`
- Use bounded quantifiers: `{1,10}` instead of `+` or `*`
- Test regex with `regex101.com` first

---

### 3. Use Filesize Checks Early

**Slow**:
```yara
condition:
    ($string1 and $string2) and filesize < 1MB
```

**Fast**:
```yara
condition:
    filesize < 1MB and ($string1 and $string2)
```

**Rationale**: Filesize check is instant, string matching is slow. Check size first.

---

### 4. Use String Modifiers Wisely

**Slow**:
```yara
strings:
    $s = "test" xor  // Checks all 256 XOR keys
```

**Fast**:
```yara
strings:
    $s = "test" ascii  // Only checks plain ASCII
```

**XOR modifier cost**: 256x slower (checks every single-byte XOR key)

---

### 5. Optimize String Sets

**Slow**:
```yara
strings:
    $s1 = "pattern1"
    $s2 = "pattern2"
    ... (100 strings)
    $s100 = "pattern100"
condition:
    any of them  // Checks all 100 strings
```

**Fast**:
```yara
strings:
    $s1 = "pattern1"
    $s2 = "pattern2"
    $s3 = "pattern3"
condition:
    2 of them  // Can stop after finding 2
```

**Guideline**: Use specific counts when possible (`2 of them` vs `any of them`).

---

### 6. Benchmark Your Rules

Use `yara` command-line tool to benchmark:

```bash
# Time a rule on a test file
time yara my_rule.yar test_file.exe

# Verbose output shows which strings matched
yara -s my_rule.yar test_file.exe

# Profile rule performance
yara --profiling my_rule.yar large_file.bin
```

**Target Performance**:
- Simple rules: < 10ms per MB
- Complex rules: < 50ms per MB
- Very complex: < 100ms per MB

**Warning**: Rules taking > 1 second per MB should be optimized.

---

## Testing Your Rules

### 1. Test with EICAR

Safe malware test file:

```bash
# Download EICAR test file
curl https://www.eicar.org/download/eicar.com.txt > eicar.txt

# Test your rule
yara my_rule.yar eicar.txt
```

---

### 2. Test Against Benign Files

Prevent false positives:

```bash
# Test against common legitimate files
yara my_rule.yar /usr/bin/*
yara my_rule.yar ~/Documents/*.pdf
yara my_rule.yar ~/Downloads/*

# Should NOT match legitimate files
```

---

### 3. Test Against Real Malware

**Warning**: Only do this in a safe, isolated environment (virtual machine).

```bash
# Test against malware samples
# (Obtain from malware repositories like VirusTotal, MalwareBazaar)
yara my_rule.yar malware_samples/
```

---

### 4. Integration Test in Sentinel

Test rule in actual Sentinel workflow:

1. **Add rule to Sentinel**:
   ```bash
   cp my_rule.yar ~/.local/share/Ladybird/sentinel/rules/
   pkill SentinelServer
   /path/to/SentinelServer &
   ```

2. **Download test file**:
   - Open Ladybird
   - Download EICAR or test file
   - Verify security alert appears

3. **Check logs**:
   ```bash
   tail -f ~/.local/share/Ladybird/sentinel.log
   ```

---

### 5. Continuous Testing

Create a test suite:

```bash
#!/bin/bash
# test_rules.sh

# Test against benign files (should NOT match)
echo "Testing benign files..."
yara -r rules/ /usr/bin/ && echo "ERROR: Matched benign files!"

# Test against EICAR (should match)
echo "Testing EICAR..."
yara rules/test_eicar.yar eicar.txt || echo "ERROR: Didn't match EICAR!"

# Test against malware samples (should match)
echo "Testing malware..."
yara -r rules/ malware_samples/ || echo "ERROR: Didn't match malware!"

echo "All tests passed!"
```

---

## Community Rules

### Finding Rules

**1. Official YARA Rules Repository**:
- https://github.com/Yara-Rules/rules
- Curated collection of community rules
- Organized by malware family and type

**2. VirusTotal YARA**:
- https://github.com/VirusTotal/yara
- Official YARA repository with example rules
- Includes modules documentation

**3. Florian Roth's Signature Base**:
- https://github.com/Neo23x0/signature-base
- High-quality rules from security researcher
- Regularly updated with new threats

**4. YARA Exchange**:
- https://github.com/Yara-Rules
- Community-contributed rules
- Various malware families

**5. ReversingLabs YARA**:
- https://github.com/reversinglabs/reversinglabs-yara-rules
- Rules from threat intelligence company

---

### Vetting Third-Party Rules

**Before using community rules, verify**:

1. **Check rule quality**:
   ```yara
   // Good: Specific patterns, low false positive risk
   rule Win32_Emotet_2023_v1 {
       meta:
           description = "Emotet variant from 2023 campaign"
           author = "Security Researcher Name"
           reference = "https://blog.example.com/emotet-analysis"
           date = "2023-06-15"
       strings:
           $specific_string = {4D 5A 90 00 03 00 00 00}
       condition:
           $specific_string
   }

   // Bad: Vague, high false positive risk
   rule Bad_File {
       strings:
           $s = "exe"
       condition:
           $s
   }
   ```

2. **Test for false positives**:
   ```bash
   # Test against your legitimate files
   yara community_rule.yar ~/Documents/
   yara community_rule.yar /usr/bin/
   ```

3. **Check metadata**:
   - Author identified?
   - Reference provided?
   - Date recent?
   - Known false positives documented?

4. **Review condition logic**:
   - Overly broad? (`any of them` with common strings)
   - Too specific? (might miss variants)
   - Reasonable file size checks?

5. **Performance test**:
   ```bash
   # Should complete quickly
   time yara community_rule.yar large_file.bin
   ```

---

### Contributing Your Rules

Share your rules with the community:

1. **Prepare rule for release**:
   ```yara
   rule My_Contribution {
       meta:
           description = "Clear description of what it detects"
           author = "Your Name <your@email.com>"
           date = "2025-10-29"
           reference = "Blog post or research paper URL"
           version = "1.0"
           tlp = "white"  // Sharing restrictions
           false_positives = "None known"

       strings:
           // Well-documented strings
           $s1 = "pattern1"  // Explanation of why this is significant

       condition:
           // Clear condition logic
           $s1
   }
   ```

2. **Document false positives**:
   ```markdown
   ## Known False Positives
   - Legitimate software X (version Y)
   - Reason: Uses similar obfuscation for DRM
   - Workaround: Whitelist specific hash
   ```

3. **Submit to repository**:
   - Fork https://github.com/Yara-Rules/rules
   - Add your rule to appropriate category
   - Submit pull request
   - Include test results and documentation

---

## Troubleshooting

### Problem: Rule Not Matching Expected Files

**Diagnosis**:

1. **Test rule directly**:
   ```bash
   yara -s my_rule.yar test_file.exe
   ```
   - `-s` shows which strings matched
   - If no output, rule didn't match

2. **Check string encoding**:
   ```yara
   // If looking for wide strings (UTF-16), add 'wide' modifier
   strings:
       $s = "string" wide ascii  // Check both encodings
   ```

3. **Verify hex patterns**:
   ```bash
   # Dump file as hex to verify pattern exists
   xxd test_file.exe | head -50

   # Search for specific hex pattern
   xxd test_file.exe | grep "4d 5a"
   ```

4. **Test condition logic**:
   ```yara
   condition:
       // Simplify to test each part
       $string1  // Test just this first
   ```

---

### Problem: Too Many False Positives

**Solutions**:

1. **Add file type checks**:
   ```yara
   condition:
       // Only match PE files
       uint16(0) == 0x5A4D and
       $suspicious_pattern
   ```

2. **Add file size constraints**:
   ```yara
   condition:
       // Typical malware size range
       filesize > 10KB and filesize < 5MB and
       $suspicious_pattern
   ```

3. **Require multiple indicators**:
   ```yara
   condition:
       // Must have 2+ suspicious patterns
       2 of ($pattern*)
   ```

4. **Use more specific strings**:
   ```yara
   // Instead of:
   $generic = "exec"

   // Use:
   $specific = "malware_specific_exec_pattern_12345"
   ```

---

### Problem: Rule Causes Slow Scans

**Solutions**:

1. **Profile the rule**:
   ```bash
   yara --profiling my_rule.yar large_file.bin
   ```

2. **Optimize regex**:
   ```yara
   // Slow:
   $regex = /(.*malware.*)+/

   // Fast:
   $regex = /malware[a-z]{0,20}/
   ```

3. **Use filesize checks early**:
   ```yara
   condition:
       filesize < 10MB and  // Check this first
       $complex_pattern
   ```

4. **Reduce string count**:
   ```yara
   // Instead of 100 strings, use 10 most specific ones
   ```

---

### Problem: Rule Won't Load

**Symptoms**: SentinelServer fails to start or logs errors

**Solutions**:

1. **Check syntax**:
   ```bash
   yara -w my_rule.yar  # Warnings
   yara --fail-on-warnings my_rule.yar
   ```

2. **Common syntax errors**:
   ```yara
   // Missing closing brace
   rule Test {
       strings:
           $s = "test"
       condition:
           $s
   // } <-- Missing this

   // Invalid hex pattern
   $hex = { 4D 5A 9G }  // 9G is not hex

   // Invalid condition
   condition:
       $nonexistent_string  // String not defined
   ```

3. **Check file permissions**:
   ```bash
   ls -la ~/.local/share/Ladybird/sentinel/rules/
   # Should be readable (at least 0644)
   ```

4. **Check logs**:
   ```bash
   tail -f ~/.local/share/Ladybird/sentinel.log
   # Look for: "Failed to compile rule: ..."
   ```

---

## Best Practices

### 1. Start Specific, Expand Carefully

Begin with highly specific patterns, then generalize:

```yara
// Start here (specific)
rule Malware_Specific_Hash {
    strings:
        $hash = {4D 5A 90 00 03 00 00 00 04 00 00 00 FF FF}
    condition:
        $hash at 0
}

// Expand to variant (more general)
rule Malware_Family {
    strings:
        $pattern1 = {4D 5A 90 00}
        $pattern2 = "malware_string"
    condition:
        all of them
}
```

---

### 2. Document Everything

```yara
rule Well_Documented_Rule {
    meta:
        description = "Detects XYZ malware family"
        author = "Your Name"
        date = "2025-10-29"
        reference = "https://blog.example.com/xyz-analysis"
        version = "1.2"

        // Sentinel-specific
        sentinel_action = "block"
        false_positives = "None known"
        last_tested = "2025-10-29"
        test_file_hash = "abc123..."

    strings:
        // Explain what each string detects
        $mz = {4D 5A}  // PE header
        $pattern = "XYZ_MALWARE_SIG"  // Unique malware signature

    condition:
        // Explain why this condition
        $mz at 0 and  // Must be PE file
        $pattern and  // Must have signature
        filesize < 2MB  // Typical size range
}
```

---

### 3. Use Meta Tags Consistently

Standard meta tags for Sentinel:

```yara
meta:
    description = "One-line description"
    author = "Name <email>"
    date = "YYYY-MM-DD"
    reference = "URL to analysis/research"
    version = "X.Y"
    tlp = "white|green|amber|red"
    severity = "critical|high|medium|low"

    // Sentinel-specific
    sentinel_action = "block|quarantine|allow"
    false_positive_risk = "low|medium|high"
    confidence = "high|medium|low"
```

---

### 4. Test Against Clean Files

**Required testing**:
```bash
# System binaries
yara my_rule.yar /usr/bin/*

# Common applications
yara my_rule.yar "/Applications/*.app"  # macOS
yara my_rule.yar "C:\Program Files\*"  # Windows

# User documents
yara my_rule.yar ~/Documents/*
yara my_rule.yar ~/Downloads/*
```

**Target**: Zero false positives on clean files.

---

### 5. Version Your Rules

```yara
rule Malware_Detection_v1 {
    meta:
        version = "1.0"
        date = "2025-10-01"
        description = "Initial version"
}

rule Malware_Detection_v2 {
    meta:
        version = "2.0"
        date = "2025-10-15"
        description = "Added support for new variant"
        changelog = "Added $pattern2 string for variant B"
}
```

Keep old versions commented out for reference:

```yara
/*
// Version 1.0 (deprecated)
rule Malware_Detection_v1 {
    ...
}
*/

// Version 2.0 (current)
rule Malware_Detection_v2 {
    ...
}
```

---

## Advanced Techniques

### Using YARA Modules

YARA modules provide extended functionality:

```yara
import "pe"
import "math"
import "hash"

rule Advanced_PE_Analysis {
    meta:
        description = "Uses PE module for advanced analysis"

    condition:
        // PE module features
        pe.is_pe and
        pe.machine == pe.MACHINE_AMD64 and
        pe.number_of_sections > 3 and
        pe.imports("kernel32.dll", "CreateRemoteThread") and

        // Math module (entropy detection)
        math.entropy(0, filesize) > 7.0 and  // High entropy (packed/encrypted)

        // Hash module
        hash.md5(0, filesize) == "d41d8cd98f00b204e9800998ecf8427e"
}
```

---

### Private Rules (Rule Reuse)

```yara
private rule IsPE {
    condition:
        uint16(0) == 0x5A4D and
        uint32(uint32(0x3C)) == 0x00004550
}

private rule HasSuspiciousImports {
    strings:
        $imp1 = "CreateRemoteThread" ascii
        $imp2 = "WriteProcessMemory" ascii
    condition:
        all of them
}

// Public rule that uses private rules
rule Trojan_Process_Injection {
    condition:
        IsPE and HasSuspiciousImports
}
```

---

### Global Rules (Apply to All)

```yara
global rule FileSizeCheck {
    condition:
        // All rules implicitly include this
        filesize < 100MB
}

rule Malware_A {
    // Automatically includes FileSizeCheck
    condition:
        ...
}
```

---

### Rule Tags

```yara
rule Malware_Example : trojan ransomware critical {
    meta:
        tags = "trojan, ransomware, critical"
    ...
}

// Match all rules with 'critical' tag
yara -t critical rules/ samples/
```

---

## Appendix: YARA Reference

### Quick Syntax Reference

```yara
// String types
$text = "string" [nocase] [wide] [ascii] [fullword]
$hex = { 4D 5A ?? [0-10] 50 45 }
$regex = /pattern/ [nocase]

// Operators
and, or, not
<, <=, >, >=, ==, !=
contains, matches

// String counts
#string > 5
@string[1]  // Offset of first match

// File properties
filesize
uint8(offset), uint16(offset), uint32(offset)

// Modules
import "pe"
import "math"
import "hash"
```

---

## Related Documentation

- [User Guide](SENTINEL_USER_GUIDE.md) - Getting started
- [Policy Guide](SENTINEL_POLICY_GUIDE.md) - Managing policies
- [Architecture Documentation](SENTINEL_ARCHITECTURE.md) - Technical details

---

**Document Information**:
- **Version**: 0.1.0
- **Last Updated**: 2025-10-29
- **Word Count**: ~7,800 words
- **Applies to**: Ladybird Sentinel Milestone 0.1
