param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [Parameter(Mandatory = $true)][string]$Dll
)
# Simulates a real install in Program Files: deny this user write access to the install folder,
# load the proxy from it, and confirm the log falls back to %LOCALAPPDATA%\MXRider instead of
# being lost. The ACL is restored in finally, so a failure cannot leave the folder unmanageable.
Add-Type -Namespace K -Name N -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr LoadLibraryW(string path);
[DllImport("kernel32.dll")]
public static extern bool FreeLibrary(IntPtr handle);
'@

$rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
    $env:USERNAME, 'Write', 'ContainerInherit,ObjectInherit', 'None', 'Deny')
$acl = Get-Acl -LiteralPath $Dir
[void]$acl.AddAccessRule($rule)
Set-Acl -LiteralPath $Dir -AclObject $acl

$fallback = Join-Path $env:LOCALAPPDATA 'MXRider\mxrider.log'
try {
    if (Test-Path -LiteralPath $fallback) { Remove-Item -LiteralPath $fallback -Force }

    # Prove the folder really refuses writes before blaming the DLL.
    try {
        Set-Content -LiteralPath (Join-Path $Dir 'probe.txt') -Value 'x' -ErrorAction Stop
        'SETUP FAILED: the folder still accepts writes, so this test proves nothing'
        exit 1
    } catch {
        'setup: writes to the install folder are refused, as in Program Files'
    }

    $handle = [K.N]::LoadLibraryW($Dll)
    if ($handle -eq [IntPtr]::Zero) {
        'LOAD_FAILED'
        exit 1
    }
    Start-Sleep -Milliseconds 400
    [void][K.N]::FreeLibrary($handle)

    if (Test-Path -LiteralPath $fallback) {
        "PASS fallback: log written to $fallback"
    } else {
        'FAIL fallback: no log anywhere, the mod would run blind'
        exit 1
    }
}
finally {
    $restore = Get-Acl -LiteralPath $Dir
    [void]$restore.RemoveAccessRule($rule)
    Set-Acl -LiteralPath $Dir -AclObject $restore
}
