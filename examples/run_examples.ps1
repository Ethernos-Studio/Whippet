# Author: dhjs0000
#
# Build and run the examples. Windows x64 only: the generated code targets
# the MSVC ABI and is linked with clang + the Visual Studio CRT.

[CmdletBinding()]
param(
    [string]$WhippetPath = ''
)

$ErrorActionPreference = 'Stop'

function Import-MsvcEnvironment {
    if ($env:VSCMD_ARG_TGT_ARCH) {
        return
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        Write-Warning 'vswhere.exe not found; relying on the current PATH for link.exe'
        return
    }
    $installation = & $vswhere -latest -products * -property installationPath
    $installation = $installation | Select-Object -First 1
    if (-not $installation) {
        Write-Warning 'Visual Studio installation not found; relying on the current PATH'
        return
    }
    $vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) {
        throw "vcvars64.bat not found under $installation"
    }
    $lines = & cmd.exe /d /c "call `"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) {
        throw "failed to load $vcvars"
    }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
        }
    }
    if (-not $env:VSCMD_ARG_TGT_ARCH) {
        throw 'Visual Studio environment was not imported'
    }
}

Import-MsvcEnvironment

$root = Split-Path -Parent $PSScriptRoot
if ($WhippetPath) {
    $whippet = $WhippetPath
} else {
    $whippet = Join-Path $root 'build\whippet.exe'
}
if (-not (Test-Path -LiteralPath $whippet)) {
    throw "whippet executable not found at $whippet; build the project first (see README.md)"
}

$clangCommand = Get-Command clang -ErrorAction SilentlyContinue
if ($clangCommand) {
    $clang = $clangCommand.Source
} else {
    $fallback = 'E:\LLVM\bin\clang.exe'
    if (Test-Path -LiteralPath $fallback) {
        $clang = $fallback
    } else {
        throw 'clang not found (install LLVM or add clang to PATH)'
    }
}

$outputDir = Join-Path $root 'build\examples'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

function Invoke-Example {
    param(
        [string]$Name,
        [string]$IrFile,
        [string]$Driver,
        [int]$ExpectedExit
    )
    $ir = Join-Path $root "examples\$IrFile"
    $asm = Join-Path $outputDir "$Name.s"
    $exe = Join-Path $outputDir "$Name.exe"

    & $whippet -S -o $asm $ir
    if ($LASTEXITCODE -ne 0) {
        throw "${Name}: whippet failed with exit code $LASTEXITCODE"
    }

    if ($Driver) {
        $driverPath = Join-Path $root "examples\$Driver"
        & $clang $driverPath $asm -o $exe
    } else {
        & $clang $asm -o $exe
    }
    if ($LASTEXITCODE -ne 0) {
        throw "${Name}: clang assemble/link failed; run from a VS developer shell if link.exe is missing"
    }

    Write-Host "--- $Name ---"
    & $exe
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne $ExpectedExit) {
        throw "${Name}: expected exit code $ExpectedExit, got $exitCode"
    }
    Write-Host "ok $Name"
}

Invoke-Example -Name 'add_mul' -IrFile 'add_mul.wir' -Driver 'add_mul_driver.c' -ExpectedExit 0
Invoke-Example -Name 'fib' -IrFile 'fib.wir' -Driver 'fib_driver.c' -ExpectedExit 0
Invoke-Example -Name 'fib_rec' -IrFile 'fib_rec.wir' -Driver 'fib_rec_driver.c' -ExpectedExit 0
Invoke-Example -Name 'classify' -IrFile 'classify.wir' -Driver 'classify_driver.c' -ExpectedExit 0
Invoke-Example -Name 'bitops' -IrFile 'bitops.wir' -Driver 'bitops_driver.c' -ExpectedExit 0
Invoke-Example -Name 'pointers' -IrFile 'pointers.wir' -Driver 'pointers_driver.c' -ExpectedExit 0
Invoke-Example -Name 'hello' -IrFile 'hello.wir' -Driver '' -ExpectedExit 0
Invoke-Example -Name 'exit42' -IrFile 'exit42.wir' -Driver '' -ExpectedExit 42

Write-Host 'all examples passed'
