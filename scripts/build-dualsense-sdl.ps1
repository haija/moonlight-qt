param(
    [Parameter(Mandatory = $true)]
    [string]$DependenciesRoot,

    [ValidateSet('x64', 'arm64')]
    [string[]]$TargetArchitectures = @('x64', 'arm64')
)

$ErrorActionPreference = 'Stop'

$SdlRepository = 'https://github.com/Taveszfito/SDL.git'
$SdlRevision = 'c1dd6604bc7607bcd46b79e1bc6f8d5a19c90124'
$CompatRepository = 'https://github.com/libsdl-org/sdl2-compat.git'
$CompatRevision = 'a53b6ad90ecd2d0ccfe01d5cfd2059793acf8c12'
$WorkRoot = Join-Path ([System.IO.Path]::GetTempPath()) "moonlight-dualsense-sdl-$PID"
$ResolvedTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$ResolvedWork = [System.IO.Path]::GetFullPath($WorkRoot)

if (!$ResolvedWork.StartsWith($ResolvedTemp, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use a build directory outside the system temporary directory: $ResolvedWork"
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable failed with exit code $LASTEXITCODE"
    }
}

try {
    New-Item -ItemType Directory -Path $ResolvedWork | Out-Null
    $SdlSource = Join-Path $ResolvedWork 'SDL'
    $CompatSource = Join-Path $ResolvedWork 'sdl2-compat'
    Invoke-Checked -Executable git -Arguments @('clone', '--filter=blob:none', $SdlRepository, $SdlSource)
    Invoke-Checked -Executable git -Arguments @('-C', $SdlSource, 'checkout', $SdlRevision)
    Invoke-Checked -Executable git -Arguments @('clone', '--filter=blob:none', $CompatRepository, $CompatSource)
    Invoke-Checked -Executable git -Arguments @('-C', $CompatSource, 'checkout', $CompatRevision)

    $Architectures = @(
        @{ Name = 'x64'; Generator = 'x64' },
        @{ Name = 'arm64'; Generator = 'ARM64' }
    ) | Where-Object { $_.Name -in $TargetArchitectures }

    foreach ($Architecture in $Architectures) {
        $Name = $Architecture.Name
        $Generator = $Architecture.Generator
        $SdlBuild = Join-Path $ResolvedWork "sdl-build-$Name"
        $SdlStage = Join-Path $ResolvedWork "sdl-stage-$Name"
        $CompatBuild = Join-Path $ResolvedWork "compat-build-$Name"
        $CompatStage = Join-Path $ResolvedWork "compat-stage-$Name"

        Invoke-Checked -Executable cmake -Arguments @(
            '-S', $SdlSource, '-B', $SdlBuild, '-A', $Generator,
            "-DCMAKE_INSTALL_PREFIX=$SdlStage",
            '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TEST_LIBRARY=OFF',
            '-DSDL_TESTS=OFF', '-DSDL_INSTALL_TESTS=OFF', '-DSDL_INSTALL_DOCS=OFF'
        )
        Invoke-Checked -Executable cmake -Arguments @(
            '--build', $SdlBuild, '--config', 'Release', '--target', 'INSTALL', '--parallel', '2'
        )

        Invoke-Checked -Executable cmake -Arguments @(
            '-S', $CompatSource, '-B', $CompatBuild, '-A', $Generator,
            "-DCMAKE_PREFIX_PATH=$SdlStage",
            "-DSDL3_INCLUDE_DIRS=$SdlStage\include",
            "-DCMAKE_INSTALL_PREFIX=$CompatStage",
            '-DSDL2COMPAT_TESTS=OFF', '-DSDL2COMPAT_INSTALL=ON',
            '-DSDL2COMPAT_INSTALL_SDL3=ON'
        )
        Invoke-Checked -Executable cmake -Arguments @(
            '--build', $CompatBuild, '--config', 'Release', '--target', 'INSTALL', '--parallel', '2'
        )

        $Destination = Join-Path $DependenciesRoot "lib\$Name"
        Copy-Item -LiteralPath (Join-Path $CompatStage 'bin\SDL2.dll') `
            -Destination (Join-Path $Destination 'SDL2.dll') -Force
        Copy-Item -LiteralPath (Join-Path $CompatStage 'bin\SDL3.dll') `
            -Destination (Join-Path $Destination 'SDL3.dll') -Force
    }
}
finally {
    if (Test-Path -LiteralPath $ResolvedWork) {
        Remove-Item -LiteralPath $ResolvedWork -Recurse -Force
    }
}
