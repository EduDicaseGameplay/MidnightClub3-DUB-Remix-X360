# Links the game XEX (x360\build\default.xex) from the compiled runtime (build_runtime.ps1) and the compiled game
# code (build_gen.ps1). The XEX holds code recompiled from your own default.xbe: never share it.
#   powershell -ExecutionPolicy Bypass -File x360\build_xex.ps1 [-Debug]
param([switch]$Debug)
$ErrorActionPreference = 'Stop'
$X = $env:XEDK
if (-not $X) { throw 'XEDK is not set: install the Xbox 360 SDK' }
$env:PATH = "$X\bin\win32;$env:PATH"
$env:LIB = "$X\lib\xbox"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here 'build'

# MC3 X-19 (2026-11-11): .text is 35 MB, past the 32 MB reach of bl. For callers too far from the CRT's vector
# register save/restore helpers (__savevmx_N/__restvmx_N, LIBCMT crtvmx.obj, at the end of .text) the linker makes
# copies of them nearer, and those copies encode stvx128/lvx128 of vr64-vr127 wrongly (extended opcode 0x703/0x303):
# an illegal instruction on the console, which Xenia and xbGuard skip, so the vector registers came back wrong and
# the title went down later. runtime\cpu\vmx_helpers.cpp holds the same helpers (the CRT's own words, made by
# tools\make_vmx_helpers.py); their object goes in the middle of the code by size, so every caller reaches it, and
# tools\check_vmx_helpers.py stops the build if any broken copy is left.
$helpers = Join-Path $out 'rt\vmx_helpers.obj'
$rt = @(Get-ChildItem "$out\rt" -Filter *.obj | Where-Object { $_.Name -ne 'vmx_helpers.obj' })
$gen = @(Get-ChildItem "$out\gen" -Filter *.obj)
$total = ($rt + $gen | Measure-Object Length -Sum).Sum
$objs = @()
$sum = 0
$placed = $false
foreach ($o in @($rt + $gen)) {
    if (-not $placed -and $sum -ge $total / 2) { $objs += $helpers; $placed = $true }
    $objs += $o.FullName
    $sum += $o.Length
}
if (-not $placed) { $objs += $helpers }

$libs = if ($Debug) { 'xapilibd.lib','d3d9d.lib','d3dx9d.lib','xgraphicsd.lib','xaudiod2.lib','xmcored.lib','xinput2d.lib','xboxkrnl.lib' }
        else { 'xapilib.lib','d3d9.lib','d3dx9.lib','xgraphics.lib','xaudio2.lib','xmcore.lib','xinput2.lib','xboxkrnl.lib' }
& link /nologo /LTCG /DEBUG /OPT:REF /OPT:ICF /MAP:"$out\bre360.map" /OUT:"$out\bre360.exe" /PDB:"$out\bre360.pdb" @objs @libs 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
& python (Join-Path $here 'tools\check_vmx_helpers.py') "$out\bre360.pe" "$out\bre360.map"
if ($LASTEXITCODE -ne 0) { throw 'VMX helper copies with illegal instructions in the image (see above)' }
& imagexex /nologo /IN:"$out\bre360.exe" /OUT:"$out\default.xex"
if ($LASTEXITCODE -ne 0) { throw 'imagexex failed' }
Get-Item "$out\default.xex" | Select-Object FullName, Length
