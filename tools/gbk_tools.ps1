<#
 * Convert the sources of this project between GBK (codepage 936) and UTF-8.
 *
 * Every .c/.h file in main/ is stored as GBK so that Keil/VS Code on a
 * Chinese Windows shows the comments correctly; most editors here expect
 * UTF-8, so use this helper to round-trip a file when editing:
 *
 *     powershell -ExecutionPolicy Bypass -File tools\gbk_tools.ps1 `
 *                -Mode to-utf8 -Path main\GUI.c -OutFile .tmp_GUI.c
 *     ... edit .tmp_GUI.c ...
 *     powershell -ExecutionPolicy Bypass -File tools\gbk_tools.ps1 `
 *                -Mode to-gbk -Path .tmp_GUI.c -OutFile main\GUI.c
 *
 * to-gbk also normalises line endings to CRLF (the project convention) and
 * never writes a BOM, to-gbk is safe to run twice.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('to-utf8', 'to-gbk')]
    [string]$Mode,

    [Parameter(Mandatory = $true)]
    [string]$Path,

    [string]$OutFile = ''
)

$ErrorActionPreference = 'Stop'

$gbk  = [System.Text.Encoding]::GetEncoding(936)
$utf8 = New-Object System.Text.UTF8Encoding($false)

$full = (Resolve-Path -LiteralPath $Path).Path
if ([string]::IsNullOrEmpty($OutFile)) {
    $OutFile = $full
}

if ($Mode -eq 'to-utf8') {
    $text = [System.IO.File]::ReadAllText($full, $gbk)
    [System.IO.File]::WriteAllText($OutFile, $text, $utf8)
} else {
    $text = [System.IO.File]::ReadAllText($full, $utf8)
    $text = $text -replace "`r`n", "`n"
    $text = $text -replace "`n", "`r`n"
    [System.IO.File]::WriteAllText($OutFile, $text, $gbk)
}

Write-Host ("{0}: {1} -> {2} ({3} chars)" -f $Mode, $full, $OutFile, $text.Length)
