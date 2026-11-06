param([string]$Binary = (Join-Path $PSScriptRoot '..\bin\diatweaks.exe'))
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$fixture = Join-Path $repo 'bin\local-browsing-config-fixture'
New-Item -ItemType Directory -Force $fixture | Out-Null
Copy-Item -LiteralPath $Binary -Destination (Join-Path $fixture 'diatweaks.exe') -Force
Copy-Item -LiteralPath (Join-Path $repo 'tweaks.conf') -Destination (Join-Path $fixture 'tweaks.conf') -Force
$cli = Join-Path $fixture 'diatweaks.exe'
$conf = Join-Path $fixture 'tweaks.conf'
$checks = 0

function Assert-Config([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Get-LocalState {
    $entries = [regex]::Matches([IO.File]::ReadAllText($conf), '(?m)^\[localBrowsing\] (on|off)\s*$')
    if ($entries.Count -ne 1) { throw 'Expected exactly one localBrowsing entry' }
    $entries[0].Groups[1].Value
}
function Invoke-ConfigCommand([string[]]$Arguments) {
    $result = & $cli @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Configuration command failed: $Arguments" }
    $result -join "`n"
}

# These commands operate on an isolated config and never launch Dia.
Assert-Config ((Get-LocalState) -eq 'off') 'The feature must be off by default'
$null = Invoke-ConfigCommand @('enable', 'all')
Assert-Config ((Get-LocalState) -eq 'off') 'Enabling appearance patches must not opt into native accountless browsing'
$result = Invoke-ConfigCommand @('enable', 'localBrowsing')
Assert-Config ((Get-LocalState) -eq 'on') 'Explicit opt-in was not saved'
Assert-Config ($result -match 'Restart Dia') 'Native changes must report that a restart is required'
$null = Invoke-ConfigCommand @('enable', 'all')
Assert-Config ((Get-LocalState) -eq 'on') 'Enabling all must keep an existing explicit opt-in'
$null = Invoke-ConfigCommand @('merge-conf', (Join-Path $repo 'tweaks.conf'), $conf)
Assert-Config ((Get-LocalState) -eq 'on') 'A config update must preserve the saved opt-in'
$null = Invoke-ConfigCommand @('disable', 'all')
Assert-Config ((Get-LocalState) -eq 'off') 'Explicitly disabling all must disable native browsing too'
$null = Invoke-ConfigCommand @('enable', 'Bypass forced sign-in (experimental)')
Assert-Config ((Get-LocalState) -eq 'on') 'The setting must be addressable by its displayed title'
$result = Invoke-ConfigCommand @('disable', 'localBrowsing')
Assert-Config ((Get-LocalState) -eq 'off') 'Explicit disable was not saved'
Assert-Config ($result -match 'Restart Dia') 'Disabling the native setting must report its restart requirement'

# An overlapping restart must stop before inspecting or closing any browser.
$mutex = [Threading.Mutex]::new($false, 'Local\WinDiaTweaks_Launch')
$ownsMutex = $false
try {
    $ownsMutex = $mutex.WaitOne(0)
    if (-not $ownsMutex) { throw 'A real Dia launch is in progress; retry the config fixture after it finishes' }
    $start = [Diagnostics.ProcessStartInfo]::new($cli, 'restart')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.RedirectStandardInput = $true
    $child = [Diagnostics.Process]::Start($start)
    $child.StandardInput.Close()
    if (-not $child.WaitForExit(5000)) { $child.Kill(); throw 'The blocked restart did not return promptly' }
    $errorText = $child.StandardError.ReadToEnd()
    Assert-Config ($child.ExitCode -ne 0) 'An overlapping restart must fail'
    Assert-Config ($errorText -match 'Another Dia launch') 'An overlapping restart must report why it was skipped'
    $child.Dispose()
} finally {
    if ($ownsMutex) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}

Write-Host "PASS: $checks forced sign-in bypass configuration checks (no Dia launch)."
