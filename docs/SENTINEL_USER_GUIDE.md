# Sentinel User Guide

**Version**: 0.1.0 (MVP)
**Last Updated**: 2025-10-29
**Status**: Production Ready

---

## Table of Contents

1. [What is Sentinel?](#what-is-sentinel)
2. [Getting Started](#getting-started)
3. [Using the Security Center](#using-the-security-center)
4. [Understanding Security Alerts](#understanding-security-alerts)
5. [Managing Security Policies](#managing-security-policies)
6. [Working with Quarantine](#working-with-quarantine)
7. [Policy Templates](#policy-templates)
8. [Common Tasks](#common-tasks)
9. [Troubleshooting](#troubleshooting)
10. [FAQ](#faq)

---

## What is Sentinel?

Sentinel is Ladybird's integrated malware protection system that automatically scans downloads and learns from your security decisions. Unlike cloud-based security solutions, Sentinel performs **all processing locally on your machine**, ensuring your privacy while protecting you from malicious files.

### Key Features

- **Real-time Malware Detection**: Automatically scans downloads using YARA rules
- **Policy-Based Protection**: Learns from your decisions to handle similar threats automatically
- **Complete Privacy**: All scanning happens locally - no data sent to the cloud
- **User Control**: You decide what to block, allow, or quarantine
- **Threat History**: Track all security events and policy enforcement
- **Secure Quarantine**: Isolate suspicious files safely with restricted permissions

### How It Works

When you download a file, Sentinel:

1. **Scans the content** in real-time using YARA malware detection rules
2. **Checks existing policies** to see if you've made a decision about similar files before
3. **Alerts you** if a threat is detected and no policy exists
4. **Enforces your decision** by blocking, quarantining, or allowing the file
5. **Remembers your choice** if you select "Remember this decision"
6. **Logs the event** for later review in the Security Center

### Privacy Guarantee

Sentinel operates entirely on your local machine:

-  **No cloud scanning** - All YARA analysis happens locally
-  **No data collection** - Your downloads are never sent to external servers
-  **No telemetry** - We don't track what files you download or block
-  **No user accounts** - No registration or login required
-  **Open source** - All code is auditable and transparent

---

## Getting Started

### Automatic Activation

Sentinel starts automatically when you launch Ladybird. No configuration is required. A background daemon called `SentinelServer` runs to perform YARA scanning.

You can verify Sentinel is running by:

1. Open **about:security** in your browser
2. Check the "System Status" section
3. Look for "Sentinel Status: Connected"

### Your First Security Alert

When Sentinel detects a threat, you'll see a security alert dialog with:

- **File name** and download URL
- **Threat type** detected (e.g., "Win32_Trojan_Generic")
- **Severity level** (Critical, High, Medium, Low)
- **Description** of what was detected

You'll have four options:

1. **Block** - Stop the download immediately and delete the file
2. **Allow Once** - Let this specific file through this time only
3. **Always Allow** - Whitelist this file/source permanently
4. **Quarantine** - Save the file in a secure isolation area for later review

**Important**: Check the "Remember this decision" box to create a policy that will automatically handle similar threats in the future.

### Creating Your First Policy

Policies let Sentinel automatically handle threats without interrupting you. To create a policy from an alert:

1. When the security alert appears, review the threat details
2. Choose your preferred action (Block, Allow, or Quarantine)
3. **Check the "Remember this decision" box**
4. Click your chosen action button

Sentinel will now automatically apply this action to:
- Files matching the same hash (exact duplicates)
- Downloads from the same URL pattern
- Threats detected by the same YARA rule

---

## Using the Security Center

The Security Center is your command center for all security operations. Access it by typing **about:security** in the address bar.

### Dashboard Overview

The main dashboard shows four key statistics:

#### Active Policies
The number of security rules currently enforced. Each policy represents a decision you've made about how to handle specific threats or sources.

**Example**: If you have 15 active policies, Sentinel is automatically protecting you based on 15 different rules.

#### Threats Blocked
Total number of malicious downloads stopped by Sentinel since installation. This includes:
- Automatic blocks from existing policies
- Manual blocks you chose in security alerts
- Policy-based quarantine actions

#### Files Quarantined
Number of suspicious files currently in secure isolation. These files are stored with restricted permissions and can be reviewed, restored, or permanently deleted.

#### Threats Today
Security events detected in the last 24 hours. Helps you monitor recent attack attempts or accidental malware encounters.

### Navigation Tabs

The Security Center has three main sections:

1. **Dashboard** - Statistics and system status overview
2. **Policies** - View, create, edit, and delete security policies
3. **Threat History** - Browse past security events and enforcement logs

---

## Understanding Security Alerts

### Alert Components

When Sentinel displays a security alert, here's what each field means:

#### Filename
The name of the file being downloaded.
- **Example**: `installer.exe`, `document.pdf`, `archive.zip`

#### Source URL
The website or server the file came from.
- **Example**: `https://suspicious-site.ru/downloads/`
- **Why it matters**: Helps you decide if the source is trustworthy

#### Rule Name
The YARA rule that detected the threat.
- **Example**: `Win32_Trojan_Generic`, `JS_Obfuscated_Code`, `PE_Suspicious_Packer`
- **Why it matters**: Indicates what type of malware or suspicious pattern was found

#### Severity
The threat level assigned by the YARA rule:

- **Critical**: Known malware signatures - block immediately
- **High**: Suspicious patterns common in malware - investigate carefully
- **Medium**: Potentially unwanted programs (PUPs) or adware - use caution
- **Low**: Generic suspicious patterns - may be false positives

#### Description
Human-readable explanation of what was detected.
- **Example**: "This file contains patterns matching known trojan malware"

#### File Hash (SHA256)
Cryptographic fingerprint of the file's contents.
- **Why it matters**: Used for policy matching and threat tracking
- **Example**: `a3c4f7e2...` (64 hexadecimal characters)

### Making the Right Decision

Use this decision tree to choose the appropriate action:

```
Is this file from a trusted source you recognize?
├─ NO → Choose "Block" (safest option)
└─ YES → Continue to next question

Is the threat severity Critical or High?
├─ YES → Choose "Quarantine" (save for investigation) or "Block"
└─ NO → Continue to next question

Do you know this file is legitimate? (e.g., developer tools, security software)
├─ YES → Choose "Allow Once" first to verify, then "Always Allow" if safe
└─ NO → Choose "Quarantine" to investigate later
```

**Pro Tip**: When in doubt, choose **Quarantine**. You can always restore the file later if it turns out to be safe.

---

## Managing Security Policies

Policies are automated rules that tell Sentinel how to handle specific downloads without showing you an alert every time.

### Policy Types

Sentinel uses three types of pattern matching (in priority order):

#### 1. Hash-Based Policies (Highest Priority)
Matches the exact file contents using SHA256 hash.

- **Use case**: Block specific malware samples
- **Example**: Block the exact copy of `trojan.exe` you encountered
- **Precision**: 100% - only matches identical files
- **Limitation**: Doesn't catch modified versions

#### 2. URL Pattern Policies (Medium Priority)
Matches downloads from specific websites or domains.

- **Use case**: Block all downloads from known malware distribution sites
- **Example**: Block everything from `malware-cdn.ru/*`
- **Precision**: High - blocks entire sources
- **Supports wildcards**: `*.badsite.com/*`, `https://example.com/malware/*`

#### 3. Rule-Based Policies (Lowest Priority)
Matches downloads detected by specific YARA rules.

- **Use case**: Automatically quarantine all files matching a malware signature
- **Example**: Quarantine all files detected by "Win32_Trojan_Generic"
- **Precision**: Variable - depends on YARA rule quality
- **Broad coverage**: Catches many variants

### Policy Actions

Each policy specifies one of three enforcement actions:

#### Block
- **Effect**: Download is immediately cancelled and file deleted
- **User notification**: Brief notification banner (non-intrusive)
- **Use cases**:
  - Known malware hashes
  - Confirmed malicious domains
  - Critical severity threats you never want to see

#### Quarantine
- **Effect**: File is downloaded to secure isolation directory
- **Permissions**: Read-only, restricted access
- **Location**: `~/.local/share/Ladybird/Quarantine/` (Linux/macOS)
- **User notification**: Notification banner with "View Quarantine" link
- **Use cases**:
  - Suspicious files you want to investigate
  - Medium severity threats
  - Files from questionable sources

#### Allow
- **Effect**: File downloads normally without security checks
- **User notification**: None (silent pass-through)
- **Use cases**:
  - Whitelisting trusted sources
  - Allowing false positive detections
  - Developer tools or security software that triggers YARA rules

### Viewing All Policies

To see all active policies:

1. Open **about:security**
2. Click the **Policies** tab
3. Browse the list showing:
   - **Rule Name**: The YARA rule or custom label
   - **Pattern**: What the policy matches (hash, URL, or rule name)
   - **Action**: Block, Quarantine, or Allow
   - **Created**: When you created the policy
   - **Hits**: How many times this policy has been enforced

### Editing Policies

To modify an existing policy:

1. Navigate to **about:security** → **Policies**
2. Find the policy you want to change
3. Click the **Edit** button
4. Update the action, pattern, or expiration
5. Click **Save Changes**

**Note**: You cannot change a policy's match type (hash/URL/rule) after creation. Delete and recreate instead.

### Deleting Policies

To remove a policy:

1. Go to **about:security** → **Policies**
2. Find the policy to delete
3. Click the **Delete** button
4. Confirm the deletion

**Warning**: Deleting a policy means Sentinel will ask you again next time it encounters a matching threat.

---

## Working with Quarantine

Quarantine is a secure storage area for suspicious files. Files in quarantine are isolated with restricted permissions to prevent accidental execution.

### Quarantine Directory

**Location**:
- Linux/macOS: `~/.local/share/Ladybird/Quarantine/`
- Windows: `%LOCALAPPDATA%\Ladybird\Quarantine\`

**Security measures**:
- Directory permissions: `0700` (owner-only access)
- File permissions: `0400` (read-only)
- Metadata stored in separate JSON files
- Files stored with randomized names (not original filenames)

### Viewing Quarantined Files

To see all quarantined files:

1. Open **about:security**
2. Look for "Files Quarantined" count on dashboard
3. Click **"Manage Quarantine"** button (when implemented in Phase 4)

Each quarantined file shows:
- **Original filename**
- **Source URL**
- **Date quarantined**
- **File size**
- **Threat type** that triggered quarantine
- **SHA256 hash**
- **Quarantine ID** (unique identifier)

### Restoring Files from Quarantine

If you determine a quarantined file is safe (false positive):

1. Open the Quarantine Manager
2. Select the file to restore
3. Click **"Restore to Downloads"**
4. The file will be moved to your Downloads folder with proper permissions

**Tip**: After restoring, consider creating an "Allow" policy for the file's hash or source to prevent future alerts.

### Deleting Quarantined Files

To permanently remove a quarantined file:

1. Open the Quarantine Manager
2. Select the file to delete
3. Click **"Delete Permanently"**
4. Confirm the deletion

**Warning**: This action cannot be undone. The file will be permanently deleted from your system.

### Automatic Quarantine Cleanup

Sentinel automatically cleans up old quarantined files:

- **Default retention**: 30 days
- **After 30 days**: Files are automatically deleted
- **Override**: You can manually delete or restore before expiration

---

## Policy Templates

Templates provide quick starting points for common security scenarios. They're pre-configured policies you can customize.

### Available Templates

#### 1. Block Executable Downloads from Domain

**Purpose**: Prevent .exe, .msi, .bat, .dll files from specific domains

**Template configuration**:
```json
{
  "name": "Block Executables from [DOMAIN]",
  "match_pattern": {
    "url_pattern": "example.com/*",
    "mime_type": "application/*"
  },
  "action": "Block"
}
```

**When to use**:
- You encounter malware from a specific site
- You want to block all executable downloads from a domain
- Preventing accidental downloads from software mirrors

**Customization**: Replace `[DOMAIN]` with the target domain

---

#### 2. Quarantine All Downloads from Domain

**Purpose**: Automatically quarantine all files from untrusted sources

**Template configuration**:
```json
{
  "name": "Quarantine from [DOMAIN]",
  "match_pattern": {
    "url_pattern": "untrusted-site.com/*"
  },
  "action": "Quarantine"
}
```

**When to use**:
- Downloads from questionable sources you want to review
- Beta software or unofficial builds
- User-generated content platforms

**Customization**: Replace `[DOMAIN]` with the source domain

---

#### 3. Block Specific File Hash

**Purpose**: Block known malware by its cryptographic signature

**Template configuration**:
```json
{
  "name": "Block Known Malware Hash",
  "match_pattern": {
    "file_hash": "abc123..."
  },
  "action": "Block"
}
```

**When to use**:
- You have a confirmed malware hash from security research
- Sharing threat intelligence with multiple browsers
- Blocking specific files across all sources

**Customization**: Paste the SHA256 hash of the malware

---

#### 4. Allow Trusted Source

**Purpose**: Whitelist all downloads from verified safe domains

**Template configuration**:
```json
{
  "name": "Always Allow from [TRUSTED DOMAIN]",
  "match_pattern": {
    "url_pattern": "trusted-cdn.com/*"
  },
  "action": "Allow"
}
```

**When to use**:
- Official software distribution sites (e.g., github.com, microsoft.com)
- Your organization's internal CDN
- Reducing false positives from developer tools

**Customization**: Replace `[TRUSTED DOMAIN]` with the verified domain

---

### Using Templates

To create a policy from a template:

1. Open **about:security**
2. Click **"Create from Template"** button
3. Select a template from the list
4. Fill in the customization fields (domain, hash, etc.)
5. Review the policy details
6. Click **"Create Policy"**

### Creating Custom Patterns

Advanced users can create custom pattern combinations:

**Example: Block archives from specific domain**
```json
{
  "url_pattern": "downloads.suspicious-site.ru/*",
  "mime_type": "application/zip"
}
```

**Example: Quarantine JavaScript files with obfuscation**
```json
{
  "rule_name": "JS_Obfuscated_Code",
  "mime_type": "text/javascript"
}
```

---

## Common Tasks

### Task 1: Block All Downloads from a Malicious Website

**Scenario**: You received a phishing email with links to `malware-site.ru`

**Steps**:
1. Open **about:security**
2. Click **"Create Policy"** or **"Create from Template"**
3. Select **"Quarantine All Downloads from Domain"** template
4. In the URL pattern field, enter: `malware-site.ru/*`
5. Change action from "Quarantine" to **"Block"** (for immediate protection)
6. Click **"Create Policy"**

**Result**: All future downloads from `malware-site.ru` will be automatically blocked.

---

### Task 2: Investigate a Quarantined File

**Scenario**: Sentinel quarantined a file you think might be safe

**Steps**:
1. Open **about:security**
2. Click **"Manage Quarantine"**
3. Find the file in the list
4. Click on it to view full details:
   - Original filename and source
   - SHA256 hash
   - YARA rule that detected it
   - Threat description
5. Research the file:
   - Search the hash on VirusTotal.com
   - Check the source URL reputation
   - Review the YARA rule details
6. If safe: Click **"Restore to Downloads"**
7. If malicious: Click **"Delete Permanently"**

**Pro Tip**: After restoring, create an "Allow" policy for the hash to prevent future false positives.

---

### Task 3: Allow a False Positive Detection

**Scenario**: Sentinel blocked a legitimate developer tool

**Steps**:

**Immediate approach (one-time bypass)**:
1. When the security alert appears
2. Click **"Allow Once"**
3. The file will download normally this time

**Permanent solution**:
1. When the security alert appears
2. Check **"Remember this decision"**
3. Click **"Always Allow"**
4. Sentinel will create an allow policy

**Alternative (via Security Center)**:
1. Open **about:security** → **Policies**
2. Click **"Create Policy"**
3. Select action: **"Allow"**
4. Enter the file hash or source URL
5. Click **"Create"**

---

### Task 4: Review Security Events from Last Week

**Scenario**: You want to see all threats Sentinel detected recently

**Steps**:
1. Open **about:security**
2. Click the **"Threat History"** tab
3. Use the date range filter to select "Last 7 Days"
4. Browse the list showing:
   - **Date/Time** of detection
   - **Filename** and **Source URL**
   - **Threat type** (YARA rule name)
   - **Action taken** (Blocked, Quarantined, Allowed)
   - **Policy applied** (if any)
5. Click any entry to view full details

**Use cases**:
- Auditing security events
- Investigating suspicious activity
- Verifying policy effectiveness

---

### Task 5: Temporarily Disable Sentinel

**Scenario**: You need to download files without security checks (advanced users only)

**Steps**:
1. Open **about:preferences** (browser preferences)
2. Navigate to the **Security** section
3. Uncheck **"Enable Sentinel malware protection"**
4. Restart Ladybird

**Warning**: Disabling Sentinel removes all malware protection. Only do this if:
- You're troubleshooting Sentinel behavior
- You're downloading files from a 100% trusted source in a controlled environment
- You have alternative security measures in place

**Re-enable**: Follow the same steps and check the box again.

---

### Task 6: Export and Share Policies

**Scenario**: You want to share security policies with another machine or user

**Steps** (when implemented):
1. Open **about:security** → **Policies**
2. Select the policies to export
3. Click **"Export Selected"** button
4. Choose export format: JSON or CSV
5. Save the file
6. Transfer to another machine
7. On the other machine: **Import Policies** → select the file

**Use cases**:
- Syncing policies across multiple devices
- Sharing threat intelligence in organizations
- Backing up your security configuration

---

## Troubleshooting

### Problem: Sentinel Not Running

**Symptoms**:
- Security alerts don't appear for known malware
- about:security shows "Status: Disconnected"
- Downloads complete without scanning

**Solutions**:

1. **Check SentinelServer process**:
   ```bash
   # Linux/macOS
   ps aux | grep SentinelServer

   # Should show: /path/to/SentinelServer
   ```

2. **Manually start SentinelServer**:
   ```bash
   # Linux/macOS
   /path/to/Build/release/bin/SentinelServer &
   ```

3. **Check Unix socket**:
   ```bash
   # Linux/macOS
   ls -la /tmp/sentinel.sock

   # Should exist with permissions: srwxr-xr-x
   ```

4. **Review logs**:
   ```bash
   # Check for error messages
   tail -f ~/.local/share/Ladybird/sentinel.log
   ```

5. **Restart Ladybird**: Quit completely and relaunch

---

### Problem: Too Many False Positives

**Symptoms**:
- Legitimate files are constantly flagged as threats
- Developer tools or security software trigger alerts
- Downloads from trusted sites are blocked

**Solutions**:

1. **Review the YARA rule**:
   - Check which rule is triggering (shown in alert)
   - Some rules are intentionally broad for security research tools

2. **Create Allow policies**:
   - For specific files: Use hash-based policy
   - For trusted sources: Use URL pattern policy
   - For tool categories: Use rule-based Allow policy

3. **Disable specific YARA rules** (advanced):
   ```bash
   # Rename problematic rule file
   cd ~/.local/share/Ladybird/sentinel/rules/
   mv overly_sensitive_rule.yar overly_sensitive_rule.yar.disabled
   ```

4. **Report false positives**:
   - Submit issue with file hash and YARA rule name
   - Help improve Sentinel's accuracy

---

### Problem: Performance Impact

**Symptoms**:
- Downloads are noticeably slower
- Browser feels sluggish during downloads
- High CPU usage from SentinelServer

**Solutions**:

1. **Check SentinelServer performance**:
   ```bash
   # Monitor CPU usage
   top -p $(pgrep SentinelServer)
   ```

2. **Optimize YARA rules**:
   - Disable complex or slow rules temporarily
   - Use more efficient pattern matching
   - Limit rule set size

3. **Increase scan threshold**:
   - Configure Sentinel to skip scanning files under certain size
   - Currently scans all files (no size limit)

4. **Hardware acceleration**:
   - Ensure SentinelServer has adequate resources
   - Target: < 5% download overhead for typical files

---

### Problem: Quarantine Directory Full

**Symptoms**:
- Error message: "Cannot quarantine file: disk space"
- Large number of old quarantined files

**Solutions**:

1. **Clean up old files**:
   - Open **about:security** → **Manage Quarantine**
   - Sort by date
   - Delete files older than 7 days (if safe)

2. **Manual cleanup**:
   ```bash
   # Linux/macOS
   cd ~/.local/share/Ladybird/Quarantine/

   # List files by date
   ls -lt

   # Delete specific file
   rm quarantine_20251020_* quarantine_20251020_*.json
   ```

3. **Adjust retention policy** (when configurable):
   - Reduce automatic cleanup threshold
   - Set maximum quarantine size limit

---

### Problem: Policy Not Enforcing

**Symptoms**:
- Created a policy but still seeing alerts for matching threats
- Policy shows 0 hits despite expected matches

**Solutions**:

1. **Verify policy pattern**:
   - Check pattern syntax for typos
   - Ensure wildcards are correct: `*.example.com/*` not `*.example.com`
   - Hash must be exactly 64 hex characters (SHA256)

2. **Check policy priority**:
   - Remember: Hash > URL > Rule
   - Lower priority policy may be overridden by higher priority one
   - Example: Allow policy on rule name won't override Block policy on hash

3. **Verify policy is active**:
   - Check expiration date (if set)
   - Ensure action is correctly set (Block/Quarantine/Allow)

4. **Test pattern matching**:
   - Open **about:security** → **Policies**
   - Click policy → **"Test Pattern"**
   - Enter a sample URL/hash to verify match

---

### Problem: Database Corruption

**Symptoms**:
- Error: "Failed to open PolicyGraph database"
- Policies disappeared after crash
- about:security fails to load

**Solutions**:

1. **Verify database integrity**:
   ```bash
   # Linux/macOS
   sqlite3 ~/.local/share/Ladybird/policy_graph.db "PRAGMA integrity_check;"
   ```

2. **Backup and reset**:
   ```bash
   # Backup existing database
   cp ~/.local/share/Ladybird/policy_graph.db ~/policy_graph_backup.db

   # Remove corrupted database (will recreate on next launch)
   rm ~/.local/share/Ladybird/policy_graph.db
   ```

3. **Restore from backup**:
   ```bash
   # If you have a backup
   cp ~/policy_graph_backup.db ~/.local/share/Ladybird/policy_graph.db
   ```

4. **Manual recovery** (advanced):
   ```bash
   # Dump what's recoverable
   sqlite3 ~/.local/share/Ladybird/policy_graph.db ".recover" > recovered.sql

   # Import to new database
   sqlite3 new_policy_graph.db < recovered.sql
   ```

---

## FAQ

### General Questions

**Q: Does Sentinel send my downloads to the cloud for scanning?**

A: **No**. All scanning happens locally on your machine using YARA rules. Sentinel never sends your files or download history to any external servers. Your privacy is completely protected.

---

**Q: Can I disable Sentinel if I don't want malware protection?**

A: Yes. Go to **about:preferences** → **Security** and uncheck "Enable Sentinel malware protection". However, we recommend keeping it enabled for your safety.

---

**Q: How do I know Sentinel is working?**

A: Check **about:security** for the "System Status" section. It should show "Sentinel Status: Connected". You can also download the EICAR test file to trigger a test alert:
```
https://www.eicar.org/download/eicar.com.txt
```

---

**Q: What happens if SentinelServer crashes?**

A: Ladybird will continue working normally, but downloads won't be scanned. The browser will attempt to auto-restart SentinelServer. You can also manually restart it. Downloads will never be blocked due to Sentinel being unavailable (fail-open design for usability).

---

**Q: Can I use my own custom YARA rules?**

A: Yes! Place your `.yar` rule files in:
- Linux/macOS: `~/.local/share/Ladybird/sentinel/rules/`
- Windows: `%LOCALAPPDATA%\Ladybird\sentinel\rules\`

SentinelServer will automatically load them on next restart. See the [YARA Rule Guide](SENTINEL_YARA_RULES.md) for details.

---

### Policy Questions

**Q: What's the difference between Block and Quarantine?**

A:
- **Block**: File is immediately deleted and download cancelled. No way to recover.
- **Quarantine**: File is saved in secure isolation. You can review, restore, or permanently delete later.

Use **Block** for confirmed malware. Use **Quarantine** for suspicious files you want to investigate.

---

**Q: If I have multiple policies that match, which one is applied?**

A: Sentinel uses priority-based matching:
1. **Hash policies** (highest priority) - exact file matches
2. **URL pattern policies** (medium priority) - source-based matches
3. **Rule-based policies** (lowest priority) - YARA rule matches

The highest priority match wins. If multiple policies at the same priority level match, the most recently created policy is used.

---

**Q: Can I temporarily disable a policy without deleting it?**

A: Currently, you need to delete the policy to disable it. A "disable/enable toggle" feature is planned for a future update. As a workaround, export the policy (copy the details) before deleting, so you can recreate it later.

---

**Q: How do I share policies with other users?**

A: Policy export/import functionality is planned. For now, you can manually share:
1. Open **about:security** → **Policies**
2. Copy the policy details (pattern, action, etc.)
3. Other user creates the same policy manually
4. Or share the SQLite database file (advanced users only)

---

### Technical Questions

**Q: Where does Sentinel store its data?**

A:
- **Policy database**: `~/.local/share/Ladybird/policy_graph.db` (SQLite)
- **Quarantine directory**: `~/.local/share/Ladybird/Quarantine/`
- **YARA rules**: `~/.local/share/Ladybird/sentinel/rules/`
- **Logs**: `~/.local/share/Ladybird/sentinel.log`

---

**Q: What's the performance impact of Sentinel?**

A: Minimal. Target performance:
- < 5% overhead on download time for typical files (10MB)
- < 1% CPU usage when idle
- < 100MB memory for SentinelServer
- < 10MB additional memory for browser

Most users won't notice any performance impact.

---

**Q: Can Sentinel detect zero-day malware?**

A: Sentinel uses signature-based detection (YARA rules), which is excellent for known malware but limited for brand-new threats. It catches:
-  Known malware signatures
-  Common obfuscation patterns
-  Suspicious file structures
- ❌ Custom/novel malware without signatures

For zero-day protection, combine Sentinel with other security practices:
- Keep YARA rules updated
- Use behavioral analysis tools
- Practice safe browsing habits

---

**Q: How often are YARA rules updated?**

A: Currently, YARA rules are bundled with Ladybird releases. Automatic rule updates are planned for a future version. Advanced users can manually update rules in the `~/.local/share/Ladybird/sentinel/rules/` directory.

---

**Q: What file types does Sentinel scan?**

A: Sentinel scans **all downloads** regardless of file type. YARA rules can match:
- Executables (.exe, .dll, .so, .dylib)
- Scripts (.js, .ps1, .bat, .sh)
- Documents (.pdf, .doc, .xls)
- Archives (.zip, .rar, .7z)
- Any custom file type with appropriate YARA rules

---

### Troubleshooting Questions

**Q: Why am I getting false positives on legitimate software?**

A: Some YARA rules detect patterns common in both malware and legitimate tools:
- Packers/compressors (UPX, MPRESS)
- Obfuscation (used by both malware and DRM)
- Heuristic patterns (suspicious but not definitive)

**Solutions**:
1. Verify the file from the official source
2. Create an "Allow" policy for the file hash
3. Whitelist the official distribution domain

---

**Q: Can I recover a file after clicking "Block"?**

A: No. When you choose "Block", the file is immediately and permanently deleted. If you're unsure, always choose **"Quarantine"** instead, which allows recovery.

---

**Q: Why doesn't about:security load?**

A: Possible causes:
1. **Database corruption**: Reset database (see Troubleshooting section)
2. **Permission issues**: Check that `~/.local/share/Ladybird/` is writable
3. **Browser cache**: Clear browser cache and reload

Try:
```bash
# Check permissions
ls -la ~/.local/share/Ladybird/policy_graph.db

# Should be: -rw-r--r-- owned by your user
```

---

## Appendix: Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+Alt+S` | Open about:security (custom keyboard shortcut - may vary) |
| `Enter` | Apply default action in security alert (Block) |
| `Esc` | Close security alert (same as Allow Once) |
| `Alt+R` | Check "Remember this decision" in alert |

---

## Getting Help

If you encounter issues not covered in this guide:

1. **Check logs**: `~/.local/share/Ladybird/sentinel.log`
2. **Visit documentation**: [Ladybird Security Docs](https://ladybird.org/docs/security)
3. **Report bugs**: [GitHub Issues](https://github.com/LadybirdBrowser/ladybird/issues)
4. **Community support**: [Ladybird Discord](https://discord.gg/nvfjVJ4Svh)

---

**Document Information**:
- **Version**: 0.1.0
- **Last Updated**: 2025-10-29
- **Applies to**: Ladybird Browser with Sentinel Milestone 0.1
- **Related Docs**:
  - [Policy Management Guide](SENTINEL_POLICY_GUIDE.md)
  - [YARA Rule Guide](SENTINEL_YARA_RULES.md)
  - [Architecture Documentation](SENTINEL_ARCHITECTURE.md)
