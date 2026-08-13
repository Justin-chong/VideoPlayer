# upgrade_ffmpeg7.ps1
# Upgrade project FFmpeg from 6.x to 7.x by copying from desktop

$ErrorActionPreference = 'Stop'
$projectRoot = "F:\code\Qtcode\VideoPlayer-main"
$srcFfmpeg = Join-Path $projectRoot "src\ffmpeg"
$desktopFfmpeg = "C:\Users\Justin\Desktop\ffmpeg-master-latest-win64-gpl-shared"

Write-Host "=== Upgrading project FFmpeg 6.x -> 7.x ==="
Write-Host "Project: $srcFfmpeg"
Write-Host "Source:  $desktopFfmpeg"
Write-Host ""

# Step 1: Backup 6.x
$backupDir = Join-Path $projectRoot "src\ffmpeg_6x_backup"
if (-not (Test-Path $backupDir)) {
    Write-Host "Step 1: Backing up 6.x to $backupDir ..."
    Copy-Item -Path $srcFfmpeg -Destination $backupDir -Recurse -Force
    Write-Host "  Backup done"
} else {
    Write-Host "Step 1: Backup already exists at $backupDir"
}

# Step 2: Replace include
$dstInclude = Join-Path $srcFfmpeg "include"
$srcInclude = Join-Path $desktopFfmpeg "include"
if (Test-Path $srcInclude) {
    Write-Host ""
    Write-Host "Step 2: Replacing include with 7.x ..."
    if (Test-Path $dstInclude) {
        Remove-Item $dstInclude -Recurse -Force
    }
    Copy-Item -Path $srcInclude -Destination $dstInclude -Recurse -Force
    $count = (Get-ChildItem $dstInclude -Recurse -Filter "*.h" | Measure-Object).Count
    Write-Host "  Copied $count .h files to include/"
}

# Step 3: Replace lib
$dstLib = Join-Path $srcFfmpeg "lib"
$srcLib = Join-Path $desktopFfmpeg "lib"
if (Test-Path $srcLib) {
    Write-Host ""
    Write-Host "Step 3: Replacing lib with 7.x ..."
    if (Test-Path $dstLib) {
        Remove-Item $dstLib -Recurse -Force
    }
    Copy-Item -Path $srcLib -Destination $dstLib -Recurse -Force
    $count = (Get-ChildItem $dstLib -File | Measure-Object).Count
    Write-Host "  Copied $count files to lib/"
}

# Step 4: Replace bin
$dstBin = Join-Path $srcFfmpeg "bin"
$srcBin = Join-Path $desktopFfmpeg "bin"
if (Test-Path $srcBin) {
    Write-Host ""
    Write-Host "Step 4: Replacing bin with 7.x ..."
    if (Test-Path $dstBin) {
        Remove-Item $dstBin -Recurse -Force
    }
    Copy-Item -Path $srcBin -Destination $dstBin -Recurse -Force
    $count = (Get-ChildItem $dstBin -File | Measure-Object).Count
    Write-Host "  Copied $count files to bin/"
}

Write-Host ""
Write-Host "=== Done ==="
Write-Host ""
Write-Host "FFmpeg 7.x files in src/ffmpeg/:"
Get-ChildItem $srcFfmpeg | ForEach-Object {
    Write-Host "  $($_.Name)"
}
