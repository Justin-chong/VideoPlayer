# test_run2.ps1 - Launch VideoPlayer and check status
$buildDir = "F:\code\Qtcode\VideoPlayer-main\build\Debug"
$exe = Join-Path $buildDir "VideoPlayer.exe"

Write-Host "=== Launching: $exe ==="

# Launch
$proc = Start-Process -FilePath $exe -WorkingDirectory $buildDir -PassThru

# Wait 3 seconds
Start-Sleep -Seconds 3

# Check if exited
if ($proc.HasExited)
{
    Write-Host "Exited with code: $($proc.ExitCode)"
}
else
{
    $proc.Refresh()
    Write-Host "Still running. PID: $($proc.Id)"
    Write-Host "Window title: '$($proc.MainWindowTitle)'"
    Write-Host "Responding: $($proc.Responding)"
    Write-Host "Threads: $($proc.Threads.Count)"
    Write-Host "Working set: $([math]::Round($proc.WorkingSet64/1MB, 2)) MB"

    # Close
    Stop-Process -Id $proc.Id -Force
    Write-Host "Killed."
}
