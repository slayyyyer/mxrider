param([Parameter(Mandatory = $true)][string]$Dll)
# Loads the built opengl32 proxy into this PowerShell process, then frees it. Nothing OpenGL is
# drawn, so this exercises exactly the risky part: DllMain finding the system opengl32, patching
# every export stub, reading the ini, and getting out again without the game present.
Add-Type -Namespace K -Name N -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr LoadLibraryW(string path);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool FreeLibrary(IntPtr handle);
'@

$handle = [K.N]::LoadLibraryW($Dll)
if ($handle -eq [IntPtr]::Zero) {
    "LOAD_FAILED (error $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    exit 1
}
"LOADED at 0x{0:X}" -f $handle.ToInt64()
Start-Sleep -Milliseconds 400
"FREE " + [K.N]::FreeLibrary($handle)
