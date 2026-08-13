$buildDirs = Get-ChildItem "F:\code\Qtcode\VideoPlayer-main\build" -Directory -ErrorAction SilentlyContinue
foreach ($bdir in $buildDirs) {
    Write-Host "Found build subdir: $($bdir.FullName)"
    foreach ($dll in @("avcodec-62.dll","avformat-62.dll","avutil-60.dll","avfilter-11.dll","avdevice-62.dll","swscale-9.dll","swresample-6.dll")) {
        $src = "C:\Users\Justin\Desktop\ffmpeg-master-latest-win64-gpl-shared\bin\$dll"
        $dst = Join-Path $bdir.FullName $dll
        if (Test-Path $src) {
            Copy-Item $src $dst -Force
            Write-Host "  Copied $dll to $($bdir.FullName)"
        }
    }
    # Also delete any old 6.x DLLs that may still be in the build dir
    foreach ($oldDll in @("avcodec-61.dll","avformat-61.dll","avutil-59.dll","avfilter-10.dll","avdevice-61.dll","swscale-8.dll","swresample-5.dll","postproc-58.dll")) {
        $old = Join-Path $bdir.FullName $oldDll
        if (Test-Path $old) {
            Remove-Item $old -Force
            Write-Host "  Deleted old $oldDll from $($bdir.FullName)"
        }
    }
}
