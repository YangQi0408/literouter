param(
    [string]$Prefix = (Join-Path $env:LOCALAPPDATA 'literouter'),
    [string]$Version = 'latest',
    [string]$From = '',
    [switch]$Service,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$TaskName = 'literouter'
$Target = Join-Path $Prefix 'literouter.exe'

function Note([string]$Text) { Write-Host "  $Text" }
function Fail([string]$Text) { throw $Text }

if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $Target) { Remove-Item -LiteralPath $Target -Force }
    Note 'scheduled task and binary removed; configuration and state were kept'
    exit 0
}

$tempDir = Join-Path ([IO.Path]::GetTempPath()) ('literouter-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempDir | Out-Null
try {
    if ([string]::IsNullOrWhiteSpace($From)) {
        if ($Version -eq 'latest') {
            $url = 'https://github.com/YangQi0408/literouter/releases/latest/download/literouter-windows-x86_64.exe'
        } else {
            $tag = 'v' + $Version.TrimStart('v')
            $url = "https://github.com/YangQi0408/literouter/releases/download/$tag/literouter-$tag-windows-x86_64.exe"
        }
        $source = Join-Path $tempDir 'literouter.exe'
        Write-Host "Downloading $url"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $source
        try {
            $checksumText = Invoke-WebRequest -UseBasicParsing -Uri ($url + '.sha256')
            $expected = ($checksumText.Content -split '\s+')[0].ToLowerInvariant()
            $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash.ToLowerInvariant()
            if ([string]::IsNullOrWhiteSpace($expected) -or $expected -ne $actual) {
                Fail "checksum mismatch"
            }
            Note 'checksum verified'
        } catch {
            if ($_.Exception.Response -and $_.Exception.Response.StatusCode -eq 404) {
                Write-Warning 'no checksum asset was published; skipping verification'
            } else {
                throw
            }
        }
    } else {
        $source = (Resolve-Path -LiteralPath $From).Path
    }

    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { Fail "binary not found: $source" }
    New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
    Copy-Item -LiteralPath $source -Destination $Target -Force
    & $Target --version | Out-Host
    Note "installed $Target"

    if ($Service) {
        $action = New-ScheduledTaskAction -Execute $Target -Argument 'serve' -WorkingDirectory $Prefix
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
        $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
        $settings = New-ScheduledTaskSettingsSet -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
        Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
        Start-ScheduledTask -TaskName $TaskName
        Note 'scheduled task installed and started; it will run at login'
        Note "check it with: Get-ScheduledTask -TaskName $TaskName"
    } else {
        Note "start it with: `"$Target`" serve"
        Note "add -Service to register automatic startup at login"
    }
} finally {
    if (Test-Path -LiteralPath $tempDir) { Remove-Item -LiteralPath $tempDir -Recurse -Force }
}
