# windows/setup_msys2.ps1 - find MSYS2, or install it (no administrator rights
# needed): the build toolchain of the Windows build. Called by
# build_windows.bat; prints "MSYS2_ROOT=<folder>" as its last line.
#
#   MSYS2_ROOT=<folder>  use / install MSYS2 there, as it is (default: an
#                        existing %LOCALAPPDATA%\msys64 or C:\msys64 that is
#                        this account's, else a new C:\msys64)
param([switch]$NoUpdate)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
# messages to stderr: stdout carries only the MSYS2_ROOT= line the batch file reads
function Say([string]$m) { [Console]::Error.WriteLine($m) }

# The build runs the bash, make, gcc and libraries of the MSYS2 it finds, as
# this user. A folder found by looking (not named in MSYS2_ROOT) must be
# owned by this account, SYSTEM, Administrators or TrustedInstaller: any
# account can create C:\msys64, and one another account made first would run
# its own code here. One that other accounts can WRITE to - what C:\ passes
# on to its subfolders (Authenticated Users: Modify), so most C:\msys64
# installs - is used with a warning: refusing would stop every existing
# standard install, and the fix is one icacls command the warning shows.
$Me = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$TrustedOwners = @($Me, 'S-1-5-18', 'S-1-5-32-544',
                   'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
$BroadSids = @('S-1-1-0', 'S-1-5-11', 'S-1-5-32-545')   # Everyone, Authenticated Users, Users
# write / append / delete child / delete / change permissions / take
# ownership, and the generic rights inheritable ACEs carry
$WriteRights = 0x2 -bor 0x4 -bor 0x40 -bor 0x10000 -bor 0x40000 -bor 0x80000 -bor 0x10000000 -bor 0x40000000
# the icacls arguments that make a tree this account's (others may read and
# run it); the files inside take it on from the folder
$PrivateAcl = @('/inheritance:r', '/remove:g', '*S-1-1-0', '*S-1-5-11', '/grant:r', "*${Me}:(OI)(CI)F",
                '*S-1-5-18:(OI)(CI)F', '*S-1-5-32-544:(OI)(CI)F', '*S-1-5-32-545:(OI)(CI)RX')

function Get-OwnerName($sid) {
    try { return $sid.Translate([Security.Principal.NTAccount]).Value } catch { return $sid.Value }
}

# 'mine', 'shared' (other accounts can change it) or 'foreign:<why not>'
function Get-Trust([string]$root) {
    $shared = $false
    foreach ($p in $root, (Join-Path $root 'usr\bin'), (Join-Path $root 'usr\bin\bash.exe')) {
        try { $acl = Get-Acl -LiteralPath $p } catch { return "foreign:the permissions of $p can't be read" }
        $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier])
        if ($TrustedOwners -notcontains $owner.Value) {
            return "foreign:it belongs to $(Get-OwnerName $owner), not to this account, an administrator or the system"
        }
        foreach ($r in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
            if ($r.AccessControlType -eq 'Allow' -and ($BroadSids -contains $r.IdentityReference.Value) -and
                (([int]$r.FileSystemRights -band $WriteRights) -ne 0)) { $shared = $true }
        }
    }
    if ($shared) { return 'shared' }
    return 'mine'
}

function Test-Msys2([string]$root) { return Test-Path (Join-Path $root 'usr\bin\bash.exe') }

function Find-Msys2 {
    # named by the user: used as it is (and installed there when missing)
    if ($env:MSYS2_ROOT) {
        if (Test-Msys2 $env:MSYS2_ROOT) { return $env:MSYS2_ROOT }
        return $null
    }
    # this account's own folder first; D:\msys64 only through MSYS2_ROOT (any
    # account can create it too, and it may be a removable drive)
    foreach ($c in (Join-Path $env:LOCALAPPDATA 'msys64'), 'C:\msys64') {
        if (-not (Test-Msys2 $c)) { continue }
        $trust = Get-Trust $c
        if ($trust -eq 'mine') { return $c }
        if ($trust -eq 'shared') {
            Say "   ! $c can be changed by every account on this PC (its folder permissions): another"
            Say "     account could change the compilers and libraries the build uses. To make it this"
            Say "     account's, run once (as administrator if MSYS2 was installed as administrator):"
            $acl = ($PrivateAcl | ForEach-Object { if ($_.StartsWith('*')) { "`"$_`"" } else { $_ } }) -join ' '
            Say "       icacls `"$c`" $acl"
            Say "     or set MSYS2_ROOT=$c to use it as it is without this warning."
            return $c
        }
        Say "   ! $c is not used: $($trust.Substring(8))"
        Say "     - another account may have put it there. If it is the MSYS2 to use, set"
        Say "     MSYS2_ROOT=$c and run build_windows.bat again."
        $script:Refused = $c
    }
    return $null
}

function Invoke-Msys([string]$root, [string]$cmd) {
    $env:MSYSTEM = 'UCRT64'
    $env:CHERE_INVOKING = '1'
    & (Join-Path $root 'usr\bin\bash.exe') -lc $cmd
    return $LASTEXITCODE
}

$root = Find-Msys2
if (-not $root) {
    # no second MSYS2 behind the back of one in D:\msys64 either
    if (-not $env:MSYS2_ROOT -and (Test-Msys2 'D:\msys64')) {
        Say "Error: D:\msys64 holds an MSYS2, which is only used when named (any account can create it, and"
        Say "       it may be a removable drive): set MSYS2_ROOT=D:\msys64 to build with it, or"
        Say "       MSYS2_ROOT=C:\msys64 to install a new one there, and run build_windows.bat again."
        exit 1
    }
    $root = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
    # never into a folder that is there already (refused above, or not an
    # MSYS2): one someone else made would stay theirs. Only an empty one the
    # user named
    if ((Test-Path $root) -and
        (-not $env:MSYS2_ROOT -or (Get-ChildItem -Force -LiteralPath $root | Select-Object -First 1))) {
        if ($script:Refused -ne $root) { Say "   ! $root exists but holds no MSYS2." }
        Say "Error: can't install MSYS2 to $root - set MSYS2_ROOT=<a new folder> (no spaces in its path,"
        Say "       e.g. %LOCALAPPDATA%\msys64) and run build_windows.bat again."
        exit 1
    }
    $parent = Split-Path $root -Parent
    $leaf = Split-Path $root -Leaf
    Say "==> MSYS2 not found - installing it to $root (the build toolchain, ~1 GB with the packages)"
    $tmp = Join-Path $env:TEMP ('msys2-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $sfx = Join-Path $tmp 'msys2-base.sfx.exe'
    Invoke-WebRequest -Uri 'https://repo.msys2.org/distrib/msys2-x86_64-latest.sfx.exe' -OutFile $sfx
    # the archive holds a "msys64" folder
    & $sfx -y "-o$tmp" | Out-Null
    if (-not (Test-Path (Join-Path $tmp 'msys64\usr\bin\bash.exe'))) { throw "MSYS2 archive did not unpack" }
    New-Item -ItemType Directory -Force $parent | Out-Null
    if (Test-Path $root) {
        # an (empty) folder the user named: the files go into it
        Get-ChildItem -Force (Join-Path $tmp 'msys64') | Move-Item -Destination $root
    } else {
        Move-Item (Join-Path $tmp 'msys64') (Join-Path $parent $leaf)
    }
    Remove-Item -Recurse -Force $tmp
    # this account's (others may read and run it): a folder under C:\ or a
    # copy to another drive would otherwise be writable by every account
    & icacls $root @PrivateAcl /Q | Out-Null
    if ($LASTEXITCODE -ne 0) { Say "   ! could not set the permissions of $root (icacls exit $LASTEXITCODE)" }
    # first start: keyring + profile, then the core update (twice: the first
    # one may update pacman / the msys runtime itself and stop there)
    Invoke-Msys $root 'exit 0' | Out-Null
    Invoke-Msys $root 'pacman -Syuu --noconfirm' | Out-Null
    Invoke-Msys $root 'pacman -Syuu --noconfirm' | Out-Null
    Say "   MSYS2 installed: $root"
} elseif (-not $NoUpdate) {
    Say "==> Using MSYS2 at $root"
}
"MSYS2_ROOT=$root"
