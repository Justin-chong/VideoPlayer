$src = "C:\Users\Justin\Desktop\ffmpeg-master-latest-win64-gpl-shared\bin"
$dst = "F:\code\Qtcode\VideoPlayer-main\src\ffmpeg\bin"

# Create dst if it doesn't exist
New-Item -ItemType Directory -Path $dst -Force | Out-Null

# Copy the 7 DLLs (overwrite)
Copy-Item "$src\avcodec-62.dll"   $dst -Force
Copy-Item "$src\avformat-62.dll"  $dst -Force
Copy-Item "$src\avutil-60.dll"    $dst -Force
Copy-Item "$src\avfilter-11.dll"  $dst -Force
Copy-Item "$src\avdevice-62.dll"  $dst -Force
Copy-Item "$src\swscale-9.dll"    $dst -Force
Copy-Item "$src\swresample-6.dll" $dst -Force

# Verify - list the new file sizes
Write-Host "=== DLL files now in project src/ffmpeg/bin ==="
Get-ChildItem "$dst\*.dll" | Select-Object Name, @{Name='Size_MB';Expression={[math]::Round($_.Length/1MB, 2)}} | Format-Table -AutoSize
