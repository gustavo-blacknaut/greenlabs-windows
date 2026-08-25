# Build sem depender do vcvars64.bat.
#
# O vcvars monta o ambiente rodando uma cadeia de .bat que trava em terminal
# nao interativo. Como os caminhos sao fixos, montar INCLUDE/LIB/PATH aqui e
# equivalente e sempre termina.
#
#   .\build.ps1              -> Release
#   .\build.ps1 -Config Debug

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [switch]$Limpar
)

$ErrorActionPreference = 'Stop'

$vs   = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools'
$sdk  = 'C:\Program Files (x86)\Windows Kits\10'

$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory |
        Sort-Object Name -Descending | Select-Object -First 1
$sdkV = Get-ChildItem "$sdk\Include" -Directory |
        Sort-Object Name -Descending | Select-Object -First 1

if (-not $msvc) { throw "MSVC nao encontrado em $vs" }
if (-not $sdkV) { throw "Windows SDK nao encontrado em $sdk" }

Write-Host "MSVC $($msvc.Name)  |  SDK $($sdkV.Name)  |  $Config"

$env:INCLUDE = @(
    "$($msvc.FullName)\include"
    "$sdk\Include\$($sdkV.Name)\ucrt"
    "$sdk\Include\$($sdkV.Name)\um"
    "$sdk\Include\$($sdkV.Name)\shared"
    "$sdk\Include\$($sdkV.Name)\winrt"
    "$sdk\Include\$($sdkV.Name)\cppwinrt"
) -join ';'

$env:LIB = @(
    "$($msvc.FullName)\lib\x64"
    "$sdk\Lib\$($sdkV.Name)\ucrt\x64"
    "$sdk\Lib\$($sdkV.Name)\um\x64"
) -join ';'

$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$cl    = "$($msvc.FullName)\bin\Hostx64\x64\cl.exe"

$env:PATH = "$($msvc.FullName)\bin\Hostx64\x64;$sdk\bin\$($sdkV.Name)\x64;$env:PATH"

$saida = Join-Path $PSScriptRoot "build\$Config"
if ($Limpar -and (Test-Path $saida)) { Remove-Item $saida -Recurse -Force }

& $cmake -S $PSScriptRoot -B $saida -G Ninja `
    -DCMAKE_MAKE_PROGRAM="$ninja" `
    -DCMAKE_C_COMPILER="$cl" `
    -DCMAKE_CXX_COMPILER="$cl" `
    "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw "configuracao do CMake falhou" }

& $cmake --build $saida
if ($LASTEXITCODE -ne 0) { throw "compilacao falhou" }

Write-Host ""
Write-Host "Pronto: $saida\bin"
Get-ChildItem "$saida\bin" -Filter *.exe | ForEach-Object {
    '{0,-24} {1,8:N0} bytes' -f $_.Name, $_.Length
}
