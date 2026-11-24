param(
    [string]$Compiler = "",
    [string]$Strip = "",
    [string]$RuntimeDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $RuntimeDir) {
    $RuntimeDir = Join-Path (Split-Path -Parent $root) "speed-roll-runtime"
}
$outputName = "speed roll.exe"
if (-not $Compiler) {
    $compilerCommand = Get-Command g++ -ErrorAction SilentlyContinue
    if ($compilerCommand) {
        $Compiler = $compilerCommand.Source
    } else {
        throw "g++ not found. Install MinGW-w64 or pass -Compiler <path-to-g++.exe>."
    }
}
if (-not $Strip) {
    $Strip = Join-Path (Split-Path -Parent $Compiler) "strip.exe"
}

$compileArgs = @(
    "-std=c++17",
    "-Os",
    "-s",
    "-mwindows",
    "-municode",
    "-static",
    "-static-libgcc",
    "-static-libstdc++",
    "-finput-charset=UTF-8",
    "-ffunction-sections",
    "-fdata-sections",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-threadsafe-statics",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wl,--gc-sections",
    "-Wl,--dynamicbase",
    "-Wl,--nxcompat",
    "-Wl,--high-entropy-va",
    "-o", $outputName,
    "main.cpp",
    "-luser32",
    "-lshell32",
    "-lgdi32",
    "-limm32",
    "-ladvapi32",
    "-lpsapi",
    "-lcomctl32"
)

Push-Location $root
try {
    & $Compiler @compileArgs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    if (Test-Path -LiteralPath $Strip) {
        & $Strip $outputName
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
    $file = Get-Item -LiteralPath $outputName
    $sizeKb = [math]::Round($file.Length / 1KB, 2)
    Write-Host ("Built: {0}" -f $file.FullName)
    Write-Host ("Size : {0} bytes ({1} KB)" -f $file.Length, $sizeKb)

    # The session workspace can carry a low integrity label. A copy in a normal
    # directory runs at medium integrity, which the auto-start registry write
    # and cross-process name lookup require.
    if ($RuntimeDir) {
        New-Item -ItemType Directory -Force -Path $RuntimeDir | Out-Null
        $runtimeExe = Join-Path $RuntimeDir $outputName
        Copy-Item -LiteralPath $file.FullName -Destination $runtimeExe -Force
        Write-Host ("Run  : {0}" -f $runtimeExe)
    }
} finally {
    Pop-Location
}
