# test_run_v2.ps1
$buildDir = "F:\code\Qtcode\VideoPlayer-main\build7\Debug"
$exe = Join-Path $buildDir "VideoPlayer.exe"

Write-Host "=== Launching: $exe ==="

$proc = Start-Process -FilePath $exe -WorkingDirectory $buildDir -PassThru
Start-Sleep -Seconds 5

if ($proc.HasExited)
{
    Write-Host "Exited at 5s with code: $($proc.ExitCode)"
    $hexCode = "{0:X}" -f ([uint32]$proc.ExitCode)
    Write-Host "Hex: 0x$hexCode"
}
else
{
    $proc.Refresh()
    Write-Host "Still running. PID: $($proc.Id)"
    Write-Host "Window handle: $($proc.MainWindowHandle)"
    Write-Host "Window title: '$($proc.MainWindowTitle)'"
    Write-Host "Responding: $($proc.Responding)"
    Write-Host "Threads: $($proc.Threads.Count)"
    Write-Host "Working set: $([math]::Round($proc.WorkingSet64/1MB, 2)) MB"

    # List all loaded FFmpeg DLLs
    $ffmpegDlls = $proc.Modules | Where-Object { $_.ModuleName -match "^(av|sw|post)" }
    Write-Host ""
    Write-Host "FFmpeg modules loaded:"
    foreach ($m in $ffmpegDlls) {
        Write-Host ("  {0,-20} {1:N0} bytes" -f $m.ModuleName, $m.ModuleMemorySize)
    }

    Stop-Process -Id $proc.Id -Force
    Write-Host ""
    Write-Host "Killed."
}
