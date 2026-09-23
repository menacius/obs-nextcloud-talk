[CmdletBinding()]
param(
    [string]$ObsSource = "",
    [string]$ObsBuild = "",
    [string]$Dependencies = "",
    [string]$PthreadsDirectory = "",
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "RelWithDebInfo",
    [string]$BuildDirectory = "build"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$desktopRoot = Resolve-Path (Join-Path $projectRoot "..\..")

if (-not $ObsSource) {
    $ObsSource = Join-Path $desktopRoot "obs-studio"
}
if (-not $ObsBuild) {
    $ObsBuild = Join-Path $ObsSource "plugin_build_x64"
}

$cachePath = Join-Path $ObsBuild "CMakeCache.txt"
if (-not $Dependencies -and (Test-Path -LiteralPath $cachePath)) {
    $prefixLine = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_PREFIX_PATH:PATH=' | Select-Object -First 1
    if ($prefixLine) {
        $firstPrefix = ($prefixLine.Line -replace '^CMAKE_PREFIX_PATH:PATH=', '').Split(';')[0]
        if (Test-Path -LiteralPath $firstPrefix) {
            $Dependencies = $firstPrefix
        }
    }
}

if (-not $Dependencies) {
    throw "Could not infer the OBS Qt dependency directory. Pass -Dependencies explicitly."
}

$obsSourceResolved = (Resolve-Path -LiteralPath $ObsSource).Path
$obsBuildResolved = (Resolve-Path -LiteralPath $ObsBuild).Path
$dependenciesResolved = (Resolve-Path -LiteralPath $Dependencies).Path
$buildPath = Join-Path $projectRoot $BuildDirectory

# CMake cache values use backslashes as escapes, so normalize Windows paths.
$obsSourceCmake = $obsSourceResolved.Replace('\', '/')
$obsBuildCmake = $obsBuildResolved.Replace('\', '/')
$dependenciesCmake = $dependenciesResolved.Replace('\', '/')

$cmakeArgs = @(
    '-S', $projectRoot,
    '-B', $buildPath,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    "-Dlibobs_DIR=$obsBuildCmake/libobs",
    "-Dobs-frontend-api_DIR=$obsBuildCmake/frontend/api",
    "-DQt6_DIR=$dependenciesCmake/lib/cmake/Qt6",
    "-DCMAKE_MODULE_PATH=$obsSourceCmake/cmake/finders",
    "-DCMAKE_PREFIX_PATH=$dependenciesCmake"
)

if ($PthreadsDirectory) {
    $pthreadsResolved = (Resolve-Path -LiteralPath $PthreadsDirectory).Path.Replace('\', '/')
    $cmakeArgs += "-Dw32-pthreads_DIR=$pthreadsResolved"
}

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
}

& cmake --build $buildPath --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

$qtBin = Join-Path $dependenciesResolved "bin"
$originalPath = $env:PATH
try {
    $env:PATH = "$qtBin;$originalPath"
    & ctest --test-dir $buildPath -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "Tests failed with exit code $LASTEXITCODE"
    }
} finally {
    $env:PATH = $originalPath
}

Write-Host "Built: $(Join-Path $buildPath "$Configuration\obs-nextcloud-talk.dll")"
