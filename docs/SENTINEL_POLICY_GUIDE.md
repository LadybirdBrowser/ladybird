# Sentinel Policy Management Guide

**Version**: 0.1.0 (MVP)
**Last Updated**: 2025-10-29
**Audience**: Intermediate to Advanced Users

---

## Table of Contents

1. [Introduction to Policies](#introduction-to-policies)
2. [Policy Concepts](#policy-concepts)
3. [Policy Lifecycle](#policy-lifecycle)
4. [Creating Policies](#creating-policies)
5. [Policy Pattern Matching](#policy-pattern-matching)
6. [Advanced Policy Topics](#advanced-policy-topics)
7. [Policy Strategy Guide](#policy-strategy-guide)
8. [Best Practices](#best-practices)
9. [Policy Examples](#policy-examples)
10. [Troubleshooting](#troubleshooting)

---

## Introduction to Policies

Security policies are automated rules that define how Sentinel should handle downloads matching specific criteria. Instead of showing you a security alert every time, policies enable Sentinel to make decisions based on your previous choices.

### Why Use Policies?

**Without Policies**:
- Every threat detection requires manual intervention
- You'll see repeated alerts for the same files or sources
- Inconsistent enforcement (you might make different decisions at different times)
- Interrupts your workflow

**With Policies**:
- Automated protection based on your preferences
- Consistent enforcement across all matching threats
- Reduced alert fatigue
- Silent protection without interruptions
- Audit trail of all security decisions

### Policy Philosophy

Sentinel's policy system is built on three principles:

1. **User Control**: You define what's safe and what's not
2. **Transparency**: All policies are visible and editable
3. **Flexibility**: Policies can be broad or specific, temporary or permanent

---

## Policy Concepts

### Core Components

Every policy consists of four essential components:

#### 1. Match Pattern

Defines **what** the policy applies to. Policies can match based on:

- **File Hash (SHA256)**: Exact file content matching
  - Example: `a3c4f7e2b1d8...` (64 hex characters)
  - Precision: 100% - only matches identical files

- **URL Pattern**: Source website or domain
  - Example: `https://malware-site.ru/*`
  - Supports wildcards: `*` and `?`
  - Precision: Variable - can be broad or specific

- **Rule Name**: YARA rule that detected the threat
  - Example: `Win32_Trojan_Generic`
  - Matches all threats detected by this rule
  - Precision: Depends on rule quality

#### 2. Action

Defines **how** Sentinel should handle matching downloads:

- **Block**: Cancel download, delete file immediately
- **Quarantine**: Save file in secure isolation
- **Allow**: Download normally without scanning

#### 3. Metadata

Additional information about the policy:

- **Created At**: Timestamp when policy was created
- **Created By**: How the policy was created (user, template, import)
- **Expires At**: Optional expiration time for temporary policies
- **Hit Count**: Number of times this policy has been enforced
- **Last Hit**: Timestamp of most recent enforcement

#### 4. Priority

Determines which policy applies when multiple policies match:

1. **Hash-based** (Priority 1) - Most specific
2. **URL pattern** (Priority 2) - Medium specificity
3. **Rule-based** (Priority 3) - Least specific

**Example conflict resolution**:
```
Download: malware.exe from badsite.com
- Policy A: Hash a3c4f7... → Block
- Policy B: URL badsite.com/* → Allow
- Policy C: Rule Win32_Trojan_Generic → Quarantine

Result: Policy A (Block) wins due to highest priority
```

---

### Policy Actions in Detail

#### Block Action

**Purpose**: Immediately prevent file from being saved to disk

**Behavior**:
1. Download is cancelled mid-transfer
2. Partial file is deleted
3. User sees brief notification: "Download blocked by policy"
4. Threat is logged in history with action="Blocked"

**Best for**:
- Known malware hashes
- Confirmed malicious domains
- Critical severity threats
- Files you never want to see

**Limitations**:
- No recovery possible (file is deleted)
- Cannot be undone after enforcement

---

#### Quarantine Action

**Purpose**: Save suspicious file in secure isolation for investigation

**Behavior**:
1. Download completes normally
2. File is moved to quarantine directory:
   - Location: `~/.local/share/Ladybird/Quarantine/`
   - Renamed with unique ID: `quarantine_20251029_001`
3. Metadata JSON created with threat details
4. File permissions set to read-only (0400)
5. User sees notification: "File quarantined for review"
6. Threat is logged in history with action="Quarantined"

**Best for**:
- Medium severity threats
- Suspicious patterns that might be false positives
- Files you want to investigate later
- Sources with mixed reputation

**Recovery Options**:
- Restore to Downloads (if determined safe)
- Permanently delete (if confirmed malicious)
- Files auto-delete after 30 days (configurable)

---

#### Allow Action

**Purpose**: Whitelist downloads from trusted sources

**Behavior**:
1. Download proceeds without YARA scanning
2. No security checks performed
3. No user notification
4. File saved directly to Downloads
5. Not logged as threat (logged as "Allowed by policy")

**Best for**:
- Whitelisting official software distribution sites
- Allowing known false positives
- Trusted organizational sources
- Developer tools that trigger generic rules

**Security Warning**:
- Disables malware protection for matching downloads
- Use sparingly and only for verified safe sources
- Review allow policies regularly

---

## Policy Lifecycle

### 1. Policy Creation

Policies can be created through multiple methods:

#### From Security Alerts (Most Common)
```
User downloads file
  → Sentinel detects threat
  → Security alert dialog appears
  → User checks "Remember this decision"
  → User chooses action (Block/Quarantine/Allow)
  → Policy automatically created
```

**Resulting policy**:
- Match pattern based on threat context (hash, URL, or rule)
- Action based on user's choice
- Created By: "user_decision"
- No expiration (permanent)

---

#### From Templates
```
User opens about:security
  → Clicks "Create from Template"
  → Selects template (e.g., "Block Executables from Domain")
  → Fills in customization fields
  → Clicks "Create Policy"
  → Policy created from template
```

**Resulting policy**:
- Match pattern based on template type
- Action pre-configured in template
- Created By: "template:[template_name]"
- Expiration optional (user-defined)

---

#### Manual Creation
```
User opens about:security → Policies
  → Clicks "Create New Policy"
  → Selects match type (Hash/URL/Rule)
  → Enters pattern
  → Chooses action
  → Sets expiration (optional)
  → Clicks "Create"
```

**Resulting policy**:
- Fully customized by user
- Created By: "manual"
- All fields user-specified

---

#### Bulk Import (Advanced)
```
User exports policies from another machine
  → Transfers policy JSON file
  → Opens about:security → Policies
  → Clicks "Import Policies"
  → Selects JSON file
  → Reviews import preview
  → Confirms import
```

**Resulting policies**:
- Multiple policies created at once
- Created By: "import:[source_file]"
- Original metadata preserved

---

### 2. Policy Enforcement

When a download is initiated:

```
┌─────────────────────────────────────────────────┐
│ 1. Download starts                              │
│    RequestServer receives download request      │
└─────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────┐
│ 2. SecurityTap intercepts                       │
│    - Computes SHA256 hash of content            │
│    - Extracts URL, filename, MIME type          │
│    - Sends to SentinelServer for YARA scan      │
└─────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────┐
│ 3. YARA scanning (if threat detected)           │
│    - SentinelServer runs rules against content  │
│    - Generates alert JSON with threat details   │
│    - Returns to RequestServer                   │
└─────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────┐
│ 4. PolicyGraph query                            │
│    - Check hash-based policies first            │
│    - Check URL pattern policies second          │
│    - Check rule-based policies third            │
│    - Return first matching policy (or None)     │
└─────────────────────────────────────────────────┘
                    ↓
        ┌───────────┴───────────┐
        ↓                       ↓
┌──────────────┐        ┌──────────────────────┐
│ Policy Found │        │ No Policy            │
│              │        │ Show security alert  │
│ Auto-enforce │        │ Get user decision    │
│ - Block      │        │ Create policy if     │
│ - Quarantine │        │ "Remember" checked   │
│ - Allow      │        └──────────────────────┘
└──────────────┘
        ↓
┌─────────────────────────────────────────────────┐
│ 5. Logging                                      │
│    - Record threat in threat_history table      │
│    - Increment policy hit_count                 │
│    - Update policy last_hit timestamp           │
│    - Log enforcement details                    │
└─────────────────────────────────────────────────┘
```

---

### 3. Policy Updates

Policies can be modified after creation:

**Editable Fields**:
- Action (Block ↔ Quarantine ↔ Allow)
- Expiration date
- Notes/description (if implemented)

**Non-Editable Fields**:
- Match pattern (hash, URL, rule)
- Match type
- Created At timestamp
- Created By source

**To change match pattern**: Delete old policy and create new one

**Update Methods**:
1. Via about:security UI (click Edit button)
2. Direct database modification (advanced users only)
3. Bulk edit via policy export/import

---

### 4. Policy Expiration

Policies can have optional expiration times for temporary protection:

**Use Cases for Temporary Policies**:
- Block downloads from a site during a known compromise
- Quarantine files from a beta software release
- Allow specific tool during a development phase

**Expiration Behavior**:
```
Policy with expires_at = 2025-11-30 23:59:59

Before expiration:
  - Policy enforced normally
  - Shows "Expires in X days" in UI

After expiration:
  - Policy automatically disabled
  - No longer matches in queries
  - Remains in database for audit trail
  - Can be re-activated by removing expiration
```

**Automatic Cleanup**:
- Expired policies are retained for 90 days (audit retention)
- After 90 days, automatically deleted from database
- Can be manually deleted earlier

---

### 5. Policy Deletion

Removing policies:

**Soft Delete** (Recommended):
- Set expiration to past date
- Policy disabled but retained in database
- Can be recovered if needed
- Maintains historical context

**Hard Delete**:
- Permanently remove from database
- Cannot be recovered
- Loses all metadata (hit count, last hit)
- Breaks audit trail

**Deletion Methods**:
1. UI: about:security → Policies → Delete button
2. Bulk delete: Select multiple → Delete Selected
3. Database: Direct SQL (advanced users only)

---

## Creating Policies

### Method 1: From Security Alerts

This is the most common and recommended method for creating policies.

**Step-by-Step**:

1. **Trigger a security alert**:
   - Download a file that Sentinel detects as a threat
   - Alert dialog appears with threat details

2. **Review the threat information**:
   - Filename: `suspicious.exe`
   - URL: `https://untrusted-site.com/downloads/`
   - Rule: `Win32_Generic_Trojan`
   - Severity: `High`

3. **Decide on enforcement**:
   - Block: If definitely malicious
   - Quarantine: If unsure but suspicious
   - Allow: If false positive (verified safe)

4. **Enable policy creation**:
   - **Check the "Remember this decision" checkbox**
   - This tells Sentinel to create a policy

5. **Choose your action**:
   - Click: Block, Allow Once, Always Allow, or Quarantine

6. **Policy created automatically**:
   - Sentinel determines best match type:
     - Uses **hash** if you chose Block (most specific)
     - Uses **URL pattern** if broad protection needed
     - Uses **rule name** if pattern-based matching appropriate

**Example Resulting Policy**:
```json
{
  "id": 42,
  "rule_name": "Win32_Generic_Trojan",
  "file_hash": "a3c4f7e2b1d8...",
  "url_pattern": null,
  "mime_type": "application/x-executable",
  "action": "Block",
  "created_at": "2025-10-29T14:30:00Z",
  "created_by": "user_decision",
  "expires_at": null,
  "hit_count": 0,
  "last_hit": null
}
```

---

### Method 2: Using Policy Templates

Templates provide quick starting points for common scenarios.

**Available Templates** (as of 0.1.0):

1. Block Executables from Domain
2. Quarantine All Downloads from Domain
3. Block Specific File Hash
4. Allow Trusted Source

**Using a Template**:

1. **Open Security Center**:
   - Navigate to `about:security`

2. **Access template picker**:
   - Click "Create from Template" button
   - Template selection dialog appears

3. **Choose template**:
   - Select: "Block Executables from Domain"

4. **Customize template fields**:
   ```
   Domain: malware-distribution.ru
   File Extensions: .exe, .msi, .bat, .dll
   Action: Block (pre-filled)
   Expiration: None (permanent)
   ```

5. **Review and create**:
   - Preview shows final policy configuration
   - Click "Create Policy"

6. **Confirmation**:
   - Policy created successfully
   - Redirected to policy detail view

**Example Resulting Policy**:
```json
{
  "rule_name": "Block Executables from malware-distribution.ru",
  "url_pattern": "malware-distribution.ru/*.exe",
  "action": "Block",
  "created_by": "template:block_executables",
  "expires_at": null
}
```

---

### Method 3: Manual Policy Creation

For advanced users who want complete control.

**Step-by-Step**:

1. **Open Policy Manager**:
   - Go to `about:security` → Policies tab
   - Click "Create New Policy" button

2. **Choose match type**:
   - Options: Hash, URL Pattern, Rule Name
   - Select: **URL Pattern** (for this example)

3. **Enter match criteria**:
   ```
   URL Pattern: *.suspicious-ads.com/*
   MIME Type Filter: (optional)
   Rule Name: (leave blank for URL-only matching)
   ```

4. **Select action**:
   - Radio buttons: Block, Quarantine, Allow
   - Select: **Quarantine**

5. **Set optional fields**:
   ```
   Expiration Date: (optional) 2025-12-31
   Notes: "Quarantine ads from suspicious network"
   Priority Override: (advanced) Auto
   ```

6. **Test pattern** (optional but recommended):
   - Click "Test Pattern" button
   - Enter sample URL: `https://cdn.suspicious-ads.com/malware.js`
   - Result: **Match ✓** or **No Match ✗**

7. **Create policy**:
   - Review summary
   - Click "Create"

**Example Resulting Policy**:
```json
{
  "rule_name": "Quarantine from suspicious-ads.com",
  "url_pattern": "*.suspicious-ads.com/*",
  "action": "Quarantine",
  "created_by": "manual",
  "expires_at": "2025-12-31T23:59:59Z",
  "hit_count": 0
}
```

---

### Method 4: Bulk Import

Import multiple policies at once from JSON file.

**Export Format**:
```json
{
  "version": "1.0",
  "exported_at": "2025-10-29T14:30:00Z",
  "policies": [
    {
      "rule_name": "Block Malware Site A",
      "url_pattern": "malware-a.com/*",
      "action": "Block"
    },
    {
      "rule_name": "Block Malware Site B",
      "url_pattern": "malware-b.com/*",
      "action": "Block"
    }
  ]
}
```

**Import Process**:

1. **Prepare import file**:
   - Obtain JSON file (from export or manual creation)
   - Validate JSON syntax

2. **Open Import Dialog**:
   - Go to `about:security` → Policies
   - Click "Import Policies"

3. **Select file**:
   - Choose `.json` file
   - File is parsed and validated

4. **Review preview**:
   - Shows list of policies to be imported
   - Indicates conflicts with existing policies
   - Displays import statistics

5. **Resolve conflicts**:
   - Options: Skip, Overwrite, Create New
   - Select resolution strategy

6. **Confirm import**:
   - Click "Import X Policies"
   - Progress indicator shown

7. **Review results**:
   ```
   Import Summary:
   - Successfully imported: 15 policies
   - Skipped (duplicates): 2 policies
   - Failed (invalid): 1 policy
   ```

---

## Policy Pattern Matching

### Hash-Based Matching

**Purpose**: Match exact file contents using cryptographic hash

**Pattern Format**:
```
SHA256 hash (64 hexadecimal characters)
Example: a3c4f7e2b1d894c3f6e8a2b5d1c7f9e4a3c4f7e2b1d894c3f6e8a2b5d1c7f9e4
```

**Matching Logic**:
```sql
-- PolicyGraph database query
SELECT * FROM policies
WHERE file_hash = ? AND (expires_at IS NULL OR expires_at > NOW())
ORDER BY created_at DESC
LIMIT 1
```

**Characteristics**:
- **Precision**: 100% - only matches identical files
- **Performance**: O(1) - indexed database lookup
- **Limitations**: Doesn't catch modified versions (even 1 byte change = different hash)

**Use Cases**:
1. **Block specific malware samples**:
   ```json
   {
     "file_hash": "abc123...",
     "action": "Block"
   }
   ```

2. **Whitelist known good files**:
   ```json
   {
     "file_hash": "def456...",
     "action": "Allow"
   }
   ```

3. **Quarantine suspicious installers**:
   ```json
   {
     "file_hash": "789abc...",
     "action": "Quarantine"
   }
   ```

**Best Practices**:
- Verify hash from trusted source (e.g., VirusTotal)
- Use for confirmed malware only
- Combine with URL patterns for defense in depth

---

### URL Pattern Matching

**Purpose**: Match downloads from specific sources

**Pattern Format**:
```
URL pattern with wildcards:
- * = match any characters (including /)
- ? = match single character

Examples:
  *.malware-site.com/*          (all files from domain and subdomains)
  https://example.com/malware/* (specific directory)
  https://cdn.example.com/*.exe (specific extension)
```

**Wildcard Behavior**:
```
Pattern: *.badsite.com/*

Matches:
  ✓ https://badsite.com/file.exe
  ✓ https://www.badsite.com/file.exe
  ✓ https://cdn.badsite.com/downloads/file.exe
  ✓ http://badsite.com/file.exe (protocol doesn't matter)

Does NOT match:
  ✗ https://goodsite.com/file.exe
  ✗ https://badsite.net/file.exe (different TLD)
  ✗ https://notbadsite.com/file.exe (domain name doesn't match)
```

**Matching Logic**:
```cpp
// Simplified C++ matching code
bool matches_pattern(String const& url, String const& pattern) {
    // Convert pattern to regex
    // * → .*
    // ? → .
    // Escape other regex special chars

    auto regex = pattern
        .replace("*", ".*")
        .replace("?", ".");

    return url.matches(regex, CaseSensitivity::CaseInsensitive);
}
```

**Characteristics**:
- **Precision**: Variable - can be very specific or very broad
- **Performance**: O(n) - pattern matching on all active policies
- **Flexibility**: Can match domains, paths, file extensions

**Pattern Examples**:

**1. Match entire domain (all files)**:
```json
{
  "url_pattern": "malware-cdn.com/*",
  "action": "Block"
}
```
Blocks: `https://malware-cdn.com/anything/file.exe`

**2. Match specific subdomain**:
```json
{
  "url_pattern": "https://downloads.example.com/*",
  "action": "Quarantine"
}
```
Blocks: `https://downloads.example.com/file.zip`
Allows: `https://www.example.com/file.zip` (different subdomain)

**3. Match specific file extension**:
```json
{
  "url_pattern": "https://suspicious-site.com/*.exe",
  "action": "Block"
}
```
Blocks: `https://suspicious-site.com/installer.exe`
Allows: `https://suspicious-site.com/document.pdf`

**4. Match specific path**:
```json
{
  "url_pattern": "https://example.com/unsafe-downloads/*",
  "action": "Block"
}
```
Blocks: `https://example.com/unsafe-downloads/anything.exe`
Allows: `https://example.com/safe-downloads/file.exe`

**Best Practices**:
- Be as specific as possible to avoid false positives
- Test patterns before deploying
- Use wildcards judiciously
- Consider using subdomains for precision

---

### Rule-Based Matching

**Purpose**: Match all threats detected by specific YARA rules

**Pattern Format**:
```
YARA rule name (exact string match)
Example: Win32_Trojan_Generic
```

**Matching Logic**:
```sql
-- PolicyGraph database query
SELECT * FROM policies
WHERE rule_name = ? AND (expires_at IS NULL OR expires_at > NOW())
ORDER BY created_at DESC
LIMIT 1
```

**Characteristics**:
- **Precision**: Depends on YARA rule quality
- **Performance**: O(1) - indexed lookup
- **Coverage**: Catches many variants of same threat type

**Use Cases**:

**1. Quarantine generic obfuscation**:
```json
{
  "rule_name": "JS_Obfuscated_Code",
  "action": "Quarantine"
}
```
Rationale: Obfuscation is suspicious but might be legitimate (DRM, minification)

**2. Block known trojan family**:
```json
{
  "rule_name": "Win32_Emotet",
  "action": "Block"
}
```
Rationale: Emotet is confirmed malware, block all variants

**3. Allow security tools**:
```json
{
  "rule_name": "Security_Tool_Mimikatz",
  "action": "Allow"
}
```
Rationale: Mimikatz is a security research tool, allow for penetration testers

**YARA Rule Names** (common examples):
```
Malware:
- Win32_Trojan_Generic
- Win32_Emotet
- Win32_Ransomware
- Android_Malware
- MacOS_Trojan

Suspicious Patterns:
- JS_Obfuscated_Code
- PE_Suspicious_Packer
- PDF_Embedded_EXE
- Office_Macro_AutoOpen
- Archive_Bomb

Legitimate Tools (false positives):
- Security_Tool_Mimikatz
- Security_Tool_Metasploit
- Packer_UPX
- DRM_Protection
```

**Best Practices**:
- Understand what the YARA rule detects
- Review rule definition before creating policy
- Use Quarantine for broad/generic rules
- Use Block for specific/confirmed malware rules
- Use Allow sparingly for known false positives

---

### Combining Match Types

Policies can specify multiple match criteria (AND logic):

**Example: Hash + MIME Type**:
```json
{
  "file_hash": "abc123...",
  "mime_type": "application/x-executable",
  "action": "Block"
}
```
Only matches if BOTH hash AND MIME type match.

**Example: URL Pattern + Rule Name**:
```json
{
  "url_pattern": "*.suspicious-site.com/*",
  "rule_name": "Win32_Trojan_Generic",
  "action": "Block"
}
```
Only matches if file is from suspicious-site.com AND detected by Win32_Trojan_Generic.

**Use Cases**:
- More precise targeting
- Reduce false positives
- Defense in depth

**Limitations**:
- Overly restrictive combinations may never match
- More complex to maintain
- Harder to troubleshoot

---

## Advanced Policy Topics

### Policy Conflict Resolution

When multiple policies match a download, Sentinel uses this resolution order:

**Priority Levels**:
1. **Hash-based** (highest priority)
2. **URL pattern**
3. **Rule-based** (lowest priority)

**Within Same Priority**:
- Most recently created policy wins
- Rationale: Newer policies reflect more recent user intent

**Example Conflict**:
```
Download: malware.exe (hash: abc123...) from badsite.com
Detected by: Win32_Trojan_Generic

Active Policies:
A. Hash abc123... → Allow (created 2025-10-01)
B. URL badsite.com/* → Block (created 2025-10-15)
C. Rule Win32_Trojan_Generic → Quarantine (created 2025-10-20)

Resolution:
Policy A wins (hash has highest priority)
Action: Allow

Note: Even though policy B is more recent and blocks the URL,
policy A has higher priority due to hash matching.
```

**Viewing Conflicts**:
- about:security → Policies → "Show Conflicts" button
- Highlights policies that might contradict each other
- Suggests resolution strategies

**Resolving Conflicts**:
1. Delete lower priority policy
2. Adjust match patterns to be mutually exclusive
3. Update action to align policies
4. Add expiration to temporary policy

---

### Temporary vs Permanent Policies

**Permanent Policies**:
```json
{
  "rule_name": "Block Malware Site",
  "url_pattern": "confirmed-malware.com/*",
  "action": "Block",
  "expires_at": null
}
```
- No expiration date
- Enforced indefinitely
- Requires manual deletion to remove
- Best for: Confirmed threats, trusted whitelists

**Temporary Policies**:
```json
{
  "rule_name": "Temporary Block During Incident",
  "url_pattern": "compromised-site.com/*",
  "action": "Block",
  "expires_at": "2025-11-15T00:00:00Z"
}
```
- Expiration date set
- Automatically disabled after expiration
- Retained for audit (90 days)
- Best for: Temporary compromises, beta testing, short-term needs

**Use Cases for Temporary Policies**:

1. **Security Incident**:
   - Site was compromised but is being remediated
   - Block for 30 days while they clean up
   - Auto-expire after incident resolved

2. **Beta Software**:
   - Quarantine downloads from beta channel
   - Expire when stable release is out

3. **Development Testing**:
   - Allow specific tool during project
   - Expire when project completes

4. **Seasonal Threats**:
   - Block tax-themed malware during tax season
   - Expire after April 15

---

### Policy Export and Import

**Export Format** (JSON):
```json
{
  "version": "1.0",
  "exported_at": "2025-10-29T14:30:00Z",
  "exported_by": "user@hostname",
  "policy_count": 3,
  "policies": [
    {
      "rule_name": "Block Malware Site A",
      "url_pattern": "malware-a.com/*",
      "action": "Block",
      "created_at": "2025-10-15T10:00:00Z",
      "hit_count": 15,
      "last_hit": "2025-10-29T09:30:00Z"
    },
    {
      "rule_name": "Quarantine Suspicious Downloads",
      "rule_name_match": "JS_Obfuscated_Code",
      "action": "Quarantine",
      "created_at": "2025-10-20T14:00:00Z",
      "hit_count": 3
    }
  ]
}
```

**Export Methods**:

1. **Export All Policies**:
   - about:security → Policies → "Export All"
   - Saves all active policies to JSON file

2. **Export Selected**:
   - Select policies with checkboxes
   - Click "Export Selected"
   - Only chosen policies exported

3. **Export via CLI** (advanced):
   ```bash
   sqlite3 ~/.local/share/Ladybird/policy_graph.db \
     "SELECT * FROM policies" -json > policies.json
   ```

**Import Methods**:

1. **UI Import**:
   - about:security → Policies → "Import"
   - Select JSON file
   - Review preview
   - Confirm import

2. **Merge Strategy Options**:
   - Skip duplicates
   - Overwrite existing
   - Create all as new

3. **Validation**:
   - JSON syntax check
   - Schema validation
   - Pattern validation
   - Conflict detection

**Use Cases**:
- Sync policies across multiple machines
- Share threat intelligence in organizations
- Backup security configuration
- Version control for policies

---

### Policy Versioning and Audit Trail

Every policy change is logged for audit purposes:

**Audit Log Schema**:
```sql
CREATE TABLE policy_audit (
    id INTEGER PRIMARY KEY,
    policy_id INTEGER,
    operation TEXT, -- 'create', 'update', 'delete'
    old_value TEXT, -- JSON of old policy
    new_value TEXT, -- JSON of new policy
    changed_at INTEGER, -- Unix timestamp
    changed_by TEXT -- 'user', 'system', 'import'
);
```

**Viewing Audit Trail**:
```
about:security → Policies → Select Policy → "View History"

Policy History for "Block Malware Site":
2025-10-29 14:30:00 - Created (user_decision)
2025-10-29 15:00:00 - Updated action: Quarantine → Block (manual)
2025-10-29 16:45:00 - Hit count: 0 → 5 (enforcement)
```

**Reverting Changes**:
- Select audit log entry
- Click "Revert to This Version"
- Policy restored to previous state

---

## Policy Strategy Guide

### Strategy 1: Defense in Depth

Layer multiple policies for comprehensive protection:

```
Layer 1: Hash-based blocks for known malware
  Policy: Block hash abc123... (confirmed trojan)

Layer 2: URL pattern blocks for malicious domains
  Policy: Block *.malware-cdn.com/* (known bad CDN)

Layer 3: Rule-based quarantine for suspicious patterns
  Policy: Quarantine rule JS_Obfuscated_Code (suspicious but not confirmed)

Layer 4: Whitelist trusted sources
  Policy: Allow github.com/* (verified safe source)
```

**Benefits**:
- Multiple layers of protection
- Catches threats missed by one layer
- Reduces false positives (whitelist override)

---

### Strategy 2: Progressive Enforcement

Start with quarantine, escalate to block:

```
Week 1: Quarantine rule Win32_Generic_Suspicious
  - Collect samples
  - Analyze false positive rate

Week 2: Review quarantined files
  - Confirmed malware: Create hash-based blocks
  - False positives: Create allow policies

Week 3: Upgrade policy to Block
  - Change action: Quarantine → Block
  - Only after confirming accuracy
```

**Benefits**:
- Reduces false positives
- Allows investigation before blocking
- Data-driven policy refinement

---

### Strategy 3: Source-Based Trust Model

Categorize sources and apply appropriate policies:

```
Trusted Sources (Allow):
- github.com/*
- microsoft.com/*
- apple.com/*

Untrusted Sources (Quarantine):
- Unknown domains
- User-uploaded content sites
- Freeware hosting sites

Malicious Sources (Block):
- Confirmed malware distribution
- Phishing domains
- Compromised CDNs
```

**Implementation**:
```json
[
  {
    "rule_name": "Allow GitHub",
    "url_pattern": "*.github.com/*",
    "action": "Allow"
  },
  {
    "rule_name": "Quarantine Freeware Sites",
    "url_pattern": "*.free-software-hosting.com/*",
    "action": "Quarantine"
  },
  {
    "rule_name": "Block Malware CDN",
    "url_pattern": "*.malware-cdn.ru/*",
    "action": "Block"
  }
]
```

---

### Strategy 4: Threat Intelligence Integration

Leverage external threat feeds:

```
1. Subscribe to threat intelligence feed
2. Receive daily malware hash list
3. Bulk import as block policies
4. Auto-update with latest threats
```

**Example Workflow**:
```bash
# Download threat feed
curl https://threat-feed.example.com/hashes.json > threats.json

# Convert to policy format
jq '.hashes[] | {rule_name: "Threat Intel Block", file_hash: ., action: "Block"}' \
  threats.json > policies.json

# Import to Sentinel
# (via about:security → Import Policies)
```

---

## Best Practices

### DO: Start Specific, Expand Carefully

**Good**:
```json
{
  "url_pattern": "malware-site.com/bad-directory/*",
  "action": "Block"
}
```

**Bad**:
```json
{
  "url_pattern": "*",
  "action": "Block"
}
```
This would block ALL downloads!

---

### DO: Use Quarantine for Uncertainty

When unsure if something is malicious:
- ✓ Use **Quarantine** action
- ✓ Investigate later
- ✓ Restore if safe, delete if malicious

Never use **Block** unless confirmed malware.

---

### DO: Test Patterns Before Deployment

Use the pattern tester:
```
about:security → Create Policy → Test Pattern

Pattern: *.example.com/*
Test URL: https://cdn.example.com/file.exe
Result: ✓ Match

Test URL: https://example.net/file.exe
Result: ✗ No Match
```

---

### DO: Document Policy Intent

Add notes to policies explaining why they exist:
```json
{
  "rule_name": "Block Malware Site X",
  "notes": "Created 2025-10-29 after phishing campaign. Site confirmed by VirusTotal."
}
```

---

### DO: Review Policies Regularly

Monthly policy review checklist:
- [ ] Check for expired policies
- [ ] Review hit counts (are policies effective?)
- [ ] Remove outdated policies
- [ ] Update patterns if needed
- [ ] Check for conflicts

---

### DON'T: Over-Whitelist

Avoid creating too many Allow policies:
- Each Allow policy disables scanning
- Creates security gaps
- Hard to maintain

**Guideline**: Allow policies should be < 10% of total policies

---

### DON'T: Ignore Hit Counts

If a policy has 0 hits after 30 days:
- May be ineffective (pattern too specific)
- May be obsolete (threat no longer active)
- Consider revising or deleting

---

### DON'T: Mix Policy Purposes

**Bad**: One policy for multiple unrelated threats
```json
{
  "rule_name": "Block Various Bad Stuff",
  "url_pattern": "*.badsite.com/* OR *.otherbad.net/*"
}
```

**Good**: Separate policies for each threat
```json
[
  {
    "rule_name": "Block Bad Site A",
    "url_pattern": "*.badsite.com/*"
  },
  {
    "rule_name": "Block Bad Site B",
    "url_pattern": "*.otherbad.net/*"
  }
]
```

Benefits:
- Easier to manage
- Independent hit tracking
- Simpler to modify/delete

---

## Policy Examples

### Example 1: Block Known Malware Hash

**Scenario**: VirusTotal confirmed hash `abc123...` is Emotet trojan

**Policy**:
```json
{
  "rule_name": "Block Emotet Trojan (VirusTotal confirmed)",
  "file_hash": "abc123def456...",
  "action": "Block",
  "created_by": "manual",
  "notes": "Confirmed via VirusTotal 2025-10-29"
}
```

**Effectiveness**: High (100% precision for this exact file)

---

### Example 2: Quarantine Downloads from Compromised Site

**Scenario**: news-site.com was compromised, serving malware for unknown duration

**Policy**:
```json
{
  "rule_name": "Quarantine from Compromised News Site",
  "url_pattern": "*.news-site.com/*",
  "action": "Quarantine",
  "expires_at": "2025-11-30T23:59:59Z",
  "notes": "Site compromised 2025-10-29. Expires when confirmed clean."
}
```

**Effectiveness**: Medium (may quarantine legitimate content too)
**Temporary**: Expires after 30 days or when site is verified clean

---

### Example 3: Allow GitHub Releases

**Scenario**: Frequent false positives on developer tools from GitHub

**Policy**:
```json
{
  "rule_name": "Allow GitHub Releases",
  "url_pattern": "https://github.com/*/releases/download/*",
  "action": "Allow",
  "notes": "GitHub is verified safe source. Reduces false positives on dev tools."
}
```

**Effectiveness**: High (GitHub has strong security)
**Caveat**: Malicious repos could bypass this

---

### Example 4: Block Executables from Ad Networks

**Scenario**: Ad networks shouldn't serve .exe files

**Policy**:
```json
{
  "rule_name": "Block Executables from Ad Networks",
  "url_pattern": "*.adnetwork.com/*.exe",
  "action": "Block",
  "notes": "Legitimate ads don't serve executables. Likely malvertising."
}
```

**Effectiveness**: High (very few false positives)

---

### Example 5: Quarantine Obfuscated JavaScript

**Scenario**: Obfuscated JS is suspicious but might be legitimate (DRM, minification)

**Policy**:
```json
{
  "rule_name": "Quarantine Obfuscated JavaScript",
  "rule_name_match": "JS_Obfuscated_Code",
  "action": "Quarantine",
  "notes": "JS obfuscation is suspicious but not always malicious. Review manually."
}
```

**Effectiveness**: Medium (some false positives expected)
**Rationale**: Quarantine allows investigation without blocking legitimate use

---

## Troubleshooting

### Problem: Policy Not Matching Expected Downloads

**Symptoms**:
- Created policy but still seeing alerts
- Policy hit_count remains 0

**Diagnosis**:

1. **Check pattern syntax**:
   ```
   Incorrect: example.com/*
   Correct: *.example.com/*
   ```

2. **Verify priority conflicts**:
   - Higher priority policy may override
   - Check about:security → Show Conflicts

3. **Check expiration**:
   ```sql
   sqlite3 policy_graph.db
   "SELECT * FROM policies WHERE id = 42;"

   expires_at: 2025-10-28 (already expired!)
   ```

4. **Test pattern**:
   - Use pattern tester in UI
   - Manually test with real URL

---

### Problem: Too Many Policies (Performance)

**Symptoms**:
- Slow download start times
- High CPU during policy matching
- UI lag in about:security

**Solutions**:

1. **Consolidate policies**:
   ```
   Instead of 100 individual domain blocks:
   Block *.known-malware-tld/*
   ```

2. **Delete obsolete policies**:
   - Check hit_count = 0 for 90+ days
   - Remove expired policies

3. **Use hash policies when possible**:
   - O(1) lookup vs O(n) pattern matching
   - Much faster for large policy sets

4. **Archive old policies**:
   ```bash
   # Export policies with 0 hits in last 90 days
   # Delete from active database
   # Store in archive for historical reference
   ```

---

### Problem: Policy Conflicts (Unexpected Behavior)

**Symptoms**:
- File blocked when you expected it to be allowed
- File allowed when you expected quarantine

**Diagnosis**:

1. **Check policy priority**:
   ```
   Hash policies override URL patterns
   URL patterns override rule-based
   ```

2. **Review all matching policies**:
   ```
   about:security → Threat History → Click event → "Show Matching Policies"
   ```

3. **Check policy timestamps**:
   - Newer policies of same priority win
   - You may have accidentally created conflicting policy

**Resolution**:
- Delete lower priority conflicting policy
- Update policy action to align
- Make patterns mutually exclusive

---

### Problem: Unable to Delete Policy

**Symptoms**:
- Delete button grayed out
- Error: "Policy is protected"

**Causes**:

1. **System-generated policy** (if applicable):
   - Some policies may be auto-created by system
   - Require special permissions to delete

2. **Database lock**:
   ```bash
   # Check for database locks
   lsof ~/.local/share/Ladybird/policy_graph.db
   ```

3. **Foreign key constraint**:
   - Policy may be referenced by threat history
   - Not a blocking issue in current schema

**Solutions**:
- Close all Ladybird instances
- Stop SentinelServer
- Restart browser
- Try delete again

---

## Appendix: Policy Schema Reference

### Database Schema

```sql
CREATE TABLE policies (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    rule_name TEXT NOT NULL,
    url_pattern TEXT,
    file_hash TEXT,
    mime_type TEXT,
    action TEXT NOT NULL CHECK(action IN ('Allow', 'Block', 'Quarantine')),
    created_at INTEGER NOT NULL,
    created_by TEXT NOT NULL,
    expires_at INTEGER,
    hit_count INTEGER DEFAULT 0,
    last_hit INTEGER,
    CONSTRAINT unique_pattern UNIQUE (url_pattern, file_hash, rule_name)
);

CREATE INDEX idx_policies_hash ON policies(file_hash);
CREATE INDEX idx_policies_url_pattern ON policies(url_pattern);
CREATE INDEX idx_policies_rule_name ON policies(rule_name);
CREATE INDEX idx_policies_expires_at ON policies(expires_at);
```

---

## Related Documentation

- [User Guide](SENTINEL_USER_GUIDE.md) - Getting started and basic usage
- [YARA Rules Guide](SENTINEL_YARA_RULES.md) - Custom rule creation
- [Architecture Documentation](SENTINEL_ARCHITECTURE.md) - Technical deep dive

---

**Document Information**:
- **Version**: 0.1.0
- **Last Updated**: 2025-10-29
- **Word Count**: ~8,500 words
- **Applies to**: Ladybird Sentinel Milestone 0.1
