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

# The icon and version block live in speed-roll.rc; windres turns them into a
# COFF object that is linked into the executable.
$resourceScript = Join-Path $root "speed-roll.rc"
$resourceObject = Join-Path $root "build\speed-roll.res"
$windres = Join-Path (Split-Path -Parent $Compiler) "windres.exe"
if (Test-Path -LiteralPath $resourceScript) {
    if (-not (Test-Path -LiteralPath $windres)) {
        throw "windres.exe not found next to $Compiler. MinGW-w64 is required to embed speed-roll.rc."
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resourceObject) | Out-Null
    & $windres "--codepage=65001" "-O" "coff" "-I" $root "-i" $resourceScript "-o" $resourceObject
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
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
if (Test-Path -LiteralPath $resourceObject) {
    $compileArgs += $resourceObject
}

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
