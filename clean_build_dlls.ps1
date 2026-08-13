# clean_build_dlls.ps1
# Remove renamed DLLs in build dir, leave 7.x originals

$buildDir = "F:\code\Qtcode\VideoPlayer-main\build\Debug"
$srcFfmpegBin = "F:\code\Qtcode\VideoPlayer-main\src\ffmpeg\bin"

# Delete all 6.x renamed DLLs
$renamed = @("avcodec-61.dll","avformat-61.dll","avutil-59.dll","avfilter-10.dll","avdevice-61.dll","swscale-8.dll","swresample-5.dll")
foreach ($dll in $renamed) {
    $path = Join-Path $buildDir $dll
    if (Test-Path $path) {
        Remove-Item $path -Force
        Write-Host "Deleted: $dll"
    }
}

# Delete old 7.x DLLs in build dir (to be replaced with fresh copies)
$sevenX = @("avcodec-62.dll","avformat-62.dll","avutil-60.dll","avfilter-11.dll","avdevice-62.dll","swscale-9.dll","swresample-6.dll")
foreach ($dll in $sevenX) {
    $path = Join-Path $buildDir $dll
    if (Test-Path $path) {
        Remove-Item $path -Force
        Write-Host "Deleted: $dll"
    }
}

# Copy fresh 7.x DLLs from src/ffmpeg/bin
foreach ($dll in $sevenX) {
    $src = Join-Path $srcFfmpegBin $dll
    $dst = Join-Path $buildDir $dll
    if (Test-Path $src) {
        Copy-Item $src $dst -Force
        $size = (Get-Item $dst).Length
        Write-Host "Copied: $dll ($([math]::Round($size/1MB, 2)) MB)"
    } else {
        Write-Host "MISSING: $dll"
    }
}

Write-Host ""
Write-Host "=== Final state ==="
Get-ChildItem "$buildDir\av*.dll","$buildDir\sw*.dll" | Select-Object Name, @{N='MB';E={[math]::Round($_.Length/1MB,2)}} | Format-Table -AutoSize
