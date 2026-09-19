param()

$ErrorActionPreference = 'Stop'

$fontRoot = Split-Path $PSScriptRoot -Parent
$interVersion = '4.1'
$converterVersion = '1.5.3'
$archiveName = "Inter-$interVersion.zip"
$downloadUrl = "https://github.com/rsms/inter/releases/download/v$interVersion/$archiveName"
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempDirectory = Join-Path `
    $tempBase ('HATi-ui-fonts-' + [guid]::NewGuid().ToString('N'))
$archivePath = Join-Path $tempDirectory $archiveName

$latinRanges = @(
    '0x20-0x7E',
    '0xA0-0xFF',
    '0x0100-0x017F',
    '0x2000-0x206F',
    '0x20AC'
)
$largeRanges = @(
    '0x20',
    '0x2E-0x3A',
    '0x41',
    '0x4D',
    '0x50'
)
$cyrillicRanges = @('0x0400-0x045F,0x0490-0x0491')

$targets = @(
    12, 14, 16, 20, 24, 28, 32, 40, 48, 56 |
        ForEach-Object {
            @{
                Name = "ui_font_$_"
                Size = $_
                Font = 'Inter-Regular.ttf'
                Ranges = $latinRanges
                NoCompress = $true
            }
        }
    64, 72 |
        ForEach-Object {
            @{
                Name = "ui_font_$_"
                Size = $_
                Font = 'Inter-Regular.ttf'
                Ranges = $latinRanges
                NoCompress = $false
            }
        }
    80, 96 |
        ForEach-Object {
            @{
                Name = "ui_font_$_"
                Size = $_
                Font = 'Inter-Regular.ttf'
                Ranges = $largeRanges
                NoCompress = $false
            }
        }
    @{
        Name = 'ui_font_20_semibold'
        Size = 20
        Font = 'Inter-SemiBold.ttf'
        Ranges = $latinRanges
        NoCompress = $true
    }
    14, 16, 20, 24 |
        ForEach-Object {
            @{
                Name = "ui_font_cyrillic_$_"
                Size = $_
                Font = 'Inter-Regular.ttf'
                Ranges = $cyrillicRanges
                NoCompress = $false
            }
        }
)

function Add-Argument {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$Arguments,
        [Parameter(Mandatory)]
        [string]$Name,
        [Parameter(Mandatory)]
        [string]$Value
    )

    [void]$Arguments.Add($Name)
    [void]$Arguments.Add($Value)
}

New-Item -ItemType Directory -Path $tempDirectory | Out-Null

try {
    Invoke-WebRequest -Uri $downloadUrl -OutFile $archivePath
    Expand-Archive -LiteralPath $archivePath -DestinationPath $tempDirectory

    $fontDirectory = Join-Path $tempDirectory "extras\ttf"
    $sourceLicense = Get-ChildItem -LiteralPath $tempDirectory `
        -Filter 'LICENSE.txt' -File -Recurse | Select-Object -First 1
    if (-not $sourceLicense) {
        throw 'Inter license file was not found in the downloaded archive.'
    }
    foreach ($fontName in @('Inter-Regular.ttf', 'Inter-SemiBold.ttf')) {
        $fontPath = Join-Path $fontDirectory $fontName
        if (-not (Test-Path -LiteralPath $fontPath)) {
            throw "Inter font was not found: $fontPath"
        }
    }

    foreach ($target in $targets) {
        $generatedPath = Join-Path $tempDirectory "$($target.Name).c"
        $outputPath = Join-Path $fontRoot "$($target.Name).c"
        $fontPath = Join-Path $fontDirectory $target.Font
        $arguments = [System.Collections.Generic.List[string]]::new()

        Add-Argument $arguments '--bpp' '4'
        Add-Argument $arguments '--format' 'lvgl'
        Add-Argument $arguments '--lv-include' 'lvgl.h'
        Add-Argument $arguments '--size' ([string]$target.Size)
        Add-Argument $arguments '--font' $fontPath
        if ($target.NoCompress) {
            [void]$arguments.Add('--no-compress')
        }
        foreach ($range in $target.Ranges) {
            Add-Argument $arguments '--range' $range
        }
        Add-Argument $arguments '--lv-font-name' $target.Name
        Add-Argument $arguments '--output' $generatedPath

        & npx.cmd --yes "lv_font_conv@$converterVersion" @arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to generate $($target.Name)."
        }

        $source = [IO.File]::ReadAllText($generatedPath)
        $stableOptions = [Text.StringBuilder]::new()
        [void]$stableOptions.Append(
            " * Opts: --bpp 4 --format lvgl --lv-include lvgl.h " +
            "--size $($target.Size) --font $($target.Font)")
        if ($target.NoCompress) {
            [void]$stableOptions.Append(' --no-compress')
        }
        foreach ($range in $target.Ranges) {
            [void]$stableOptions.Append(" --range $range")
        }
        [void]$stableOptions.Append(
            " --lv-font-name $($target.Name) --output $($target.Name).c")
        $source = [regex]::Replace(
            $source,
            '(?m)^ \* Opts: .+$',
            $stableOptions.ToString(),
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
            $outputPath,
            $source,
            [Text.UTF8Encoding]::new($false))
        Write-Output "Generated $outputPath"
    }

    Copy-Item -LiteralPath $sourceLicense.FullName `
        -Destination (Join-Path $fontRoot 'Inter-LICENSE.txt') -Force
    Write-Output "Copied $($sourceLicense.Name) to $fontRoot"
}
finally {
    $resolvedTemp = [IO.Path]::GetFullPath($tempDirectory)
    if ($resolvedTemp.StartsWith(
            $tempBase,
            [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path $resolvedTemp -Leaf).StartsWith(
            'HomeTiles-ui-fonts-',
            [StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
}
