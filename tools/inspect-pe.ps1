<#
.SYNOPSIS
    重新生成 README 中"二进制取证"一节的全部证据（PE 头、导入表、段熵、关键字符串）。

.DESCRIPTION
    纯静态分析：只读文件，不执行、不修改目标程序。
    在 Windows PowerShell 5.1 / PowerShell 7+ 均可运行。

.EXAMPLE
    pwsh -File .\tools\inspect-pe.ps1 -Path C:\path\to\sai2.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [switch]$FullImports
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Path)) { throw "找不到文件: $Path" }
$Path = (Resolve-Path -LiteralPath $Path).Path
$b = [System.IO.File]::ReadAllBytes($Path)

# ---- 基础校验 ---------------------------------------------------------------
if ($b[0] -ne 0x4D -or $b[1] -ne 0x5A) { throw "不是 MZ/PE 文件" }
$pe = [BitConverter]::ToInt32($b, 0x3C)
if ([BitConverter]::ToUInt32($b, $pe) -ne 0x00004550) { throw "无效的 PE 签名" }

$U16 = { param($o) [BitConverter]::ToUInt16($b, $o) }
$U32 = { param($o) [BitConverter]::ToUInt32($b, $o) }
$U64 = { param($o) [BitConverter]::ToUInt64($b, $o) }

$opt        = $pe + 0x18
$magic      = & $U16 $opt
$is64       = ($magic -eq 0x20B)
$imgBaseOff = if ($is64) { $opt + 0x18 } else { $opt + 0x1C }
$dirOff     = if ($is64) { $opt + 0x70 } else { $opt + 0x60 }
$numDirs    = if ($is64) { (& $U32 ($opt + 0x6C)) } else { (& $U32 ($opt + 0x5C)) }

Write-Host "`n=== 文件 ===" -ForegroundColor Cyan
$sha = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
"" | Select-Object @{n = 'Path'; e = { $Path } },
    @{n = 'Size'; e = { $b.Length } },
    @{n = 'SHA256'; e = { $sha } } | Format-List

Write-Host "=== PE 头 ===" -ForegroundColor Cyan
$machine = & $U16 ($pe + 4)
$machineName = switch ($machine) {
    0x8664 { 'x64 (AMD64)' }
    0x14C { 'x86 (I386)' }
    0xAA64 { 'ARM64' }
    default { ('0x{0:X}' -f $machine) }
}
[pscustomobject]@{
    Machine            = ('0x{0:X4} -> {1}' -f $machine, $machineName)
    OptionalMagic      = ('0x{0:X}' -f $magic)
    NumberOfSections   = (& $U16 ($pe + 6))
    TimeDateStamp      = ([datetimeoffset]::FromUnixTimeSeconds((& $U32 ($pe + 8))).UtcDateTime.ToString('u'))
    Subsystem          = ('{0} ({1})' -f (& $U16 ($pe + 0x5C)), $(if ((& $U16 ($pe + 0x5C)) -eq 2) { 'GUI' } else { '?' }))
    ImageBase          = $(if ($is64) { '0x{0:X}' -f (& $U64 $imgBaseOff) } else { '0x{0:X}' -f (& $U32 $imgBaseOff) })
    DllCharacteristics = ('0x{0:X4}' -f (& $U16 ($pe + 0x5E)))
    SizeOfCode         = ('0x{0:X}' -f (& $U32 ($opt + 4)))
    EntryPointRVA      = ('0x{0:X}' -f (& $U32 ($opt + 0x10)))
} | Format-List

# ---- 数据目录（重点：Reloc 是否存在）-------------------------------------
$dirNames = 'Export', 'Import', 'Resource', 'Exception', 'Security', 'Reloc', 'Debug', 'Arch',
'GlobalPtr', 'TLS', 'LoadConfig', 'BoundImport', 'IAT', 'DelayImport', 'CLR', 'Reserved'
Write-Host "=== 数据目录 ===" -ForegroundColor Cyan
$dirs = @()
for ($i = 0; $i -lt [Math]::Min(16, $numDirs); $i++) {
    $rva = & $U32 ($dirOff + $i * 8)
    $sz = & $U32 ($dirOff + $i * 8 + 4)
    $dirs += [pscustomobject]@{ Index = $i; Name = $dirNames[$i]; RVA = ('0x{0:X}' -f $rva); Size = $sz }
}
$dirs | Format-Table -AutoSize

# ---- 节表 ----------------------------------------------------------------
Write-Host "=== 节表（含熵）===" -ForegroundColor Cyan
$secOff = $opt + (& $U16 ($pe + 0x14))
$nsec = & $U16 ($pe + 6)
$secs = @()
for ($i = 0; $i -lt $nsec; $i++) {
    $o = $secOff + $i * 40
    $name = ([System.Text.Encoding]::ASCII.GetString($b, $o, 8)).Trim([char]0)
    $vsize = & $U32 ($o + 8)
    $vaddr = & $U32 ($o + 12)
    $rsize = & $U32 ($o + 16)
    $roff = & $U32 ($o + 20)

    # 熵
    $H = 0.0
    if ($rsize -gt 0) {
        $freq = New-Object 'int[]' 256
        $end = [Math]::Min($roff + $rsize, $b.Length)
        for ($k = $roff; $k -lt $end; $k++) { $freq[$b[$k]]++ }
        $len = $end - $roff
        for ($j = 0; $j -lt 256; $j++) {
            if ($freq[$j] -gt 0) { $p = $freq[$j] / $len; $H -= $p * [Math]::Log($p, 2) }
        }
    }
    $secs += [pscustomobject]@{
        Name = $name; VA = ('0x{0:X}' -f $vaddr); VSize = $vsize
        Raw = ('0x{0:X}' -f $roff); RawSize = $rsize
        Entropy = [Math]::Round($H, 3)
        Flag = $(if ($H -gt 7.2) { 'PACKED?' } else { '' })
    }
}
$secs | Format-Table -AutoSize

function Convert-RvaToOffset {
    param([uint32]$Rva)
    foreach ($s in $secs) {
        $va = [Convert]::ToUInt32($s.VA, 16)
        if ($Rva -ge $va -and $Rva -lt ($va + [Math]::Max($s.VSize, 1))) {
            return [Convert]::ToUInt32($s.Raw, 16) + ($Rva - $va)
        }
    }
    return -1
}

# ---- 导入表 --------------------------------------------------------------
Write-Host "=== 导入 DLL ===" -ForegroundColor Cyan
$impRva = $dirs[1].RVA
$impSize = $dirs[1].Size
$allImports = [ordered]@{}
if ($impSize -gt 0) {
    $o = Convert-RvaToOffset ([uint32]$impRva)
    $i = 0
    while ($true) {
        $dllRva = & $U32 ($o + $i * 20 + 12)
        if ($dllRva -eq 0) { break }
        $no = Convert-RvaToOffset $dllRva
        $e = $no; while ($b[$e] -ne 0) { $e++ }
        $dll = [System.Text.Encoding]::ASCII.GetString($b, $no, $e - $no)
        $oft = & $U32 ($o + $i * 20)
        $names = @()
        if ($oft -ne 0) {
            $fo = Convert-RvaToOffset $oft
            $k = 0
            while ($true) {
                $ent = & $U64 ($fo + $k * 8)
                if ($ent -eq 0) { break }
                $isOrdinal = if ($is64) { ($ent -band 0x8000000000000000) -ne 0 } else { ($ent -band 0x80000000) -ne 0 }
                if (-not $isOrdinal) {
                    $hn = [int]($ent -band 0x7FFFFFFF)
                    $ho = (Convert-RvaToOffset $hn) + 2
                    $he = $ho; while ($b[$he] -ne 0) { $he++ }
                    $names += [System.Text.Encoding]::ASCII.GetString($b, $ho, $he - $ho)
                }
                else { $names += ('#{0}' -f ($ent -band 0xFFFF)) }
                $k++
            }
        }
        $allImports[$dll] = $names
        $i++
    }
}
$allImports.Keys | ForEach-Object {
    [pscustomobject]@{ DLL = $_; Count = $allImports[$_].Count }
} | Sort-Object Count -Descending | Format-Table -AutoSize

if ($FullImports) {
    foreach ($k in $allImports.Keys) {
        Write-Host "--- $k ---" -ForegroundColor Yellow
        $allImports[$k] -join ', '
    }
}

# ---- 关键依赖判定 ---------------------------------------------------------
Write-Host "=== 依赖判定 ===" -ForegroundColor Cyan
$dlls = $allImports.Keys
$checks = [ordered]@{
    '需要 VC++ 运行库 (msvcr/vcruntime)' = ($dlls | Where-Object { $_ -match '(?i)^(msvcr|msvcp|vcruntime|api-ms-win-crt)' }).Count -gt 0
    '使用 Direct3D / OpenGL / DXGI'       = ($dlls | Where-Object { $_ -match '(?i)^(d3d|dxgi|opengl32|ddraw|vulkan)' }).Count -gt 0
    '使用 GDI+ / WIC 图像解码'            = ($dlls | Where-Object { $_ -match '(?i)^(gdiplus|windowscodecs)' }).Count -gt 0
    '使用 GDI32 纯软件绘制'               = $dlls -contains 'GDI32.dll'
    '有 .NET / CLR'                       = ($dirs | Where-Object Name -eq 'CLR').Size -gt 0
    '加壳迹象（.text 熵>7.2）'            = ($secs | Where-Object Name -eq '.text').Entropy -gt 7.2
}
$checks.GetEnumerator() | ForEach-Object {
    [pscustomobject]@{ 判定 = $_.Key; 结果 = $(if ($_.Value) { 'YES' } else { 'no' }) }
} | Format-Table -AutoSize

# ---- 笔输入 / 渲染能力探针 -----------------------------------------------
Write-Host "=== 能力探针（字符串 + 导入）===" -ForegroundColor Cyan
$utf16 = [System.Text.Encoding]::Unicode.GetString($b)
$ascii = [System.Text.Encoding]::ASCII.GetString($b)
$probes = [ordered]@{
    'Windows Ink (WM_POINTER)' = 'GetPointerPenInfo'
    'Tablet 消息 (WMX_TABLET)' = 'WMX_TABLET_PENPOS'
    'Wintab'                   = 'wintab32'
    'GDI 位图搬运'             = 'SetDIBitsToDevice'
    '单实例共享内存'           = 'SharedMemory'
    'SAI 原生格式 .sai2'       = '.sai2'
    '内嵌 zlib'                = 'Incomplete compressed datastream'
    '高 DPI 感知'              = 'dpiAware'
}
$probes.GetEnumerator() | ForEach-Object {
    [pscustomobject]@{
        特性   = $_.Key
        命中   = $(if ($ascii.Contains($_.Value) -or $utf16.Contains($_.Value)) { 'YES' } else { 'no' })
        证据串 = $_.Value
    }
} | Format-Table -AutoSize

Write-Host "完成。以上输出即 README 第 1 节的数据来源。`n" -ForegroundColor Green
