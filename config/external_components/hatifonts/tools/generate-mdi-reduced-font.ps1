param()

$ErrorActionPreference = 'Stop'

$fontRoot = Split-Path $PSScriptRoot -Parent
$configRoot = Split-Path (Split-Path $fontRoot -Parent) -Parent
$mdiVersion = '7.4.47'
$converterVersion = '1.5.3'
$fontName = 'mdi_icons_40_reduced'
$fontOutputPath = Join-Path $fontRoot "$fontName.c"
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempDirectory = Join-Path `
    $tempBase ('HATi-mdi-reduced-font-' + [guid]::NewGuid().ToString('N'))

$sourceFiles = Get-ChildItem -LiteralPath $configRoot -File -Recurse |
    Where-Object {
        $_.FullName -notlike '*\.esphome\*' -and
        $_.Extension -in @('.c', '.cpp', '.h', '.yaml', '.yml')
    }
$mdiMapPath = Join-Path $fontRoot 'mdi_icons.cpp'
$mdiMapSource = [IO.File]::ReadAllText($mdiMapPath)
$mapPattern = '\{"(?<name>[^"]+)",\s*0x(?<code>[0-9A-Fa-f]+)\}'
$codepointsByName = @{}
foreach ($match in [regex]::Matches($mdiMapSource, $mapPattern)) {
    $codepointsByName[$match.Groups['name'].Value] =
        [Convert]::ToInt32($match.Groups['code'].Value, 16)
}

$selectedNames = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($sourceFile in $sourceFiles) {
    $source = [IO.File]::ReadAllText($sourceFile.FullName)
    foreach ($match in [regex]::Matches(
            $source,
            'mdi:(?<name>[A-Za-z0-9-]+)|getMdiChar\(\s*"(?<name2>[^"]+)"')) {
        $name = $match.Groups['name'].Value
        if (-not $name) {
            $name = $match.Groups['name2'].Value
        }
        [void]$selectedNames.Add($name)
    }
}

foreach ($name in $codepointsByName.Keys) {
    if ($name.StartsWith('weather-', [StringComparison]::OrdinalIgnoreCase)) {
        [void]$selectedNames.Add($name)
    }
}

$missingNames = @(
    $selectedNames | Where-Object { -not $codepointsByName.ContainsKey($_) }
)
if ($missingNames.Count -gt 0) {
    throw "MDI icons were not found in mdi_icons.cpp: $($missingNames -join ', ')"
}

$codepoints = @(
    $selectedNames |
        ForEach-Object { $codepointsByName[$_] } |
        Sort-Object -Unique
)
if ($codepoints.Count -eq 0) {
    throw 'No MDI icons were selected for the reduced font.'
}

$mdiRange = ($codepoints | ForEach-Object { '0x{0:X}' -f $_ }) -join ','
Write-Output "Selected $($selectedNames.Count) icon names ($($codepoints.Count) glyphs)."

New-Item -ItemType Directory -Path $tempDirectory | Out-Null

try {
    & npm.cmd pack "@mdi/font@$mdiVersion" `
        --pack-destination $tempDirectory | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to download @mdi/font@$mdiVersion."
    }

    $archive = Get-ChildItem -LiteralPath $tempDirectory -Filter '*.tgz' |
        Select-Object -First 1
    if (-not $archive) {
        throw 'Downloaded MDI package archive was not found.'
    }

    & tar.exe -xf $archive.FullName -C $tempDirectory
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to extract the downloaded MDI package.'
    }

    $packageDirectory = Join-Path $tempDirectory 'package'
    $sourceFont = Join-Path `
        $packageDirectory 'fonts\materialdesignicons-webfont.woff'
    if (-not (Test-Path -LiteralPath $sourceFont)) {
        throw "MDI source font was not found: $sourceFont"
    }

    $generatedPath = Join-Path $tempDirectory "$fontName.c"
    & npx.cmd --yes "lv_font_conv@$converterVersion" `
        --bpp 4 `
        --size 40 `
        --font $sourceFont `
        --range $mdiRange `
        --format lvgl `
        --lv-font-name $fontName `
        --output $generatedPath
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to generate $fontName."
    }

    $source = [IO.File]::ReadAllText($generatedPath)
    $stableOptions =
        " * Opts: --bpp 4 --size 40 --font " +
        "materialdesignicons-webfont.woff --range $mdiRange " +
        "--format lvgl --lv-font-name $fontName"
    $source = [regex]::Replace(
        $source,
        '(?m)^ \* Opts: .+$',
        $stableOptions,
        1)
    $source = [regex]::Replace(
        $source,
        '(?ms)^#ifdef LV_LVGL_H_INCLUDE_SIMPLE\r?\n' +
        '#include "lvgl\.h"\r?\n#else\r?\n' +
        '#include "lvgl/lvgl\.h"\r?\n#endif',
        '#include "lvgl.h"',
        1)
    $source = $source.TrimEnd("`r", "`n") + "`n"
    [IO.File]::WriteAllText(
        $fontOutputPath,
        $source,
        [Text.UTF8Encoding]::new($false))
    Write-Output "Generated $fontOutputPath"
}
finally {
    $resolvedTemp = [IO.Path]::GetFullPath($tempDirectory)
    if ($resolvedTemp.StartsWith(
            $tempBase,
            [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path $resolvedTemp -Leaf).StartsWith(
            'HATi-mdi-reduced-font-',
            [StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
}
