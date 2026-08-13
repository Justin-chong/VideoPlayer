# test_run5.ps1 - Launch and list ALL loaded modules
$buildDir = "F:\code\Qtcode\VideoPlayer-main\build\Debug"
$exe = Join-Path $buildDir "VideoPlayer.exe"

Write-Host "=== Launching: $exe ==="

# Launch
$proc = Start-Process -FilePath $exe -WorkingDirectory $buildDir -PassThru

# Wait 2 seconds (before crash)
Start-Sleep -Seconds 2

if (-not $proc.HasExited)
{
    $proc.Refresh()
    Write-Host "Still running. PID: $($proc.Id)"
    Write-Host "Window handle: $($proc.MainWindowHandle)"
    Write-Host "Working set: $([math]::Round($proc.WorkingSet64/1MB, 2)) MB"
    Write-Host ""
    Write-Host "All loaded modules:"
    $modules = $proc.Modules | Sort-Object ModuleName
    foreach ($m in $modules) {
        Write-Host ("  {0,-50} {1:N0} bytes" -f $m.ModuleName, $m.ModuleMemorySize)
    }
    Stop-Process -Id $proc.Id -Force
    Write-Host "Killed."
}
else
{
    Write-Host "Already exited with code: $($proc.ExitCode)"
}
