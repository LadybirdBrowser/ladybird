/*
 * Sentinel Default YARA Rules
 * Copyright (c) 2025, Ladybird contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

rule EICAR_Test_File {
    meta:
        description = "EICAR anti-virus test file"
        severity = "low"
        author = "Sentinel"
    strings:
        $eicar = "EICAR-STANDARD-ANTIVIRUS-TEST-FILE"
    condition:
        $eicar
}

rule Windows_PE_Suspicious {
    meta:
        description = "Windows executable with suspicious imports"
        severity = "medium"
        author = "Sentinel"
    strings:
        $mz = {4D 5A}
        $import1 = "CreateRemoteThread" nocase
        $import2 = "VirtualAllocEx" nocase
        $import3 = "WriteProcessMemory" nocase
        $import4 = "SetWindowsHookEx" nocase
    condition:
        $mz at 0 and 2 of ($import*)
}

rule Obfuscated_JavaScript {
    meta:
        description = "Heavily obfuscated JavaScript"
        severity = "medium"
        author = "Sentinel"
    strings:
        $eval = /eval\s*\(/ nocase
        $base64 = /atob\s*\(/ nocase
        $long_str = /['"][a-zA-Z0-9+\/=]{200,}['"]/
        $unescape = /unescape\s*\(/ nocase
    condition:
        2 of them and filesize < 5MB
}

rule PowerShell_Suspicious {
    meta:
        description = "Suspicious PowerShell script indicators"
        severity = "high"
        author = "Sentinel"
    strings:
        $ps1 = ".ps1" nocase
        $invoke = "Invoke-Expression" nocase
        $invoke2 = "IEX" nocase
        $download = "DownloadString" nocase
        $download2 = "DownloadFile" nocase
        $bypass = "-ExecutionPolicy Bypass" nocase
        $hidden = "-WindowStyle Hidden" nocase
        $encode = "-EncodedCommand" nocase
        $base64_long = /[A-Za-z0-9+\/]{100,}={0,2}/
    condition:
        3 of them or ($bypass and $hidden)
}

rule Potential_Ransomware_Note {
    meta:
        description = "Potential ransomware ransom note indicators"
        severity = "critical"
        author = "Sentinel"
    strings:
        $bitcoin1 = "bitcoin" nocase
        $bitcoin2 = "BTC" nocase
        $ransom1 = "decrypt" nocase
        $ransom2 = "encrypted" nocase
        $ransom3 = "payment" nocase
        $contact1 = "@protonmail" nocase
        $contact2 = "@tutanota" nocase
        $threat1 = "publish" nocase
        $threat2 = "leak" nocase
    condition:
        (any of ($bitcoin*)) and 2 of ($ransom*) and (any of ($contact*) or any of ($threat*))
}

rule Macro_Document_Suspicious {
    meta:
        description = "Office document with suspicious macro indicators"
        severity = "high"
        author = "Sentinel"
    strings:
        $ole = {D0 CF 11 E0 A1 B1 1A E1}
        $vba = "VBA" nocase
        $auto = "AutoOpen" nocase
        $auto2 = "AutoExec" nocase
        $auto3 = "Document_Open" nocase
        $shell = "Shell" nocase
        $wscript = "WScript.Shell" nocase
        $http = "http" nocase
    condition:
        $ole at 0 and $vba and (any of ($auto*)) and (($shell or $wscript) and $http)
}

rule Executable_In_Archive {
    meta:
        description = "Executable file inside archive (common malware delivery)"
        severity = "medium"
        author = "Sentinel"
    strings:
        $zip = {50 4B 03 04}
        $rar = {52 61 72 21}
        $exe_name = ".exe" nocase
        $scr_name = ".scr" nocase
        $com_name = ".com" nocase
        $bat_name = ".bat" nocase
    condition:
        ($zip at 0 or $rar at 0) and ($exe_name or $scr_name or $com_name or $bat_name)
}
