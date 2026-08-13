# test_run3.ps1 - Launch VideoPlayer with longer wait
$buildDir = "F:\code\Qtcode\VideoPlayer-main\build\Debug"
$exe = Join-Path $buildDir "VideoPlayer.exe"

Write-Host "=== Launching: $exe ==="

# Launch
$proc = Start-Process -FilePath $exe -WorkingDirectory $buildDir -PassThru

# Wait 5 seconds for window to show
Start-Sleep -Seconds 5

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
    Write-Host "Modules:"
    $proc.Modules | Select-Object -First 20 | ForEach-Object {
        Write-Host "  $($_.ModuleName)"
    }

    # List all VideoPlayer windows
    Write-Host ""
    Write-Host "All VideoPlayer windows:"
    $wins = Get-Process -Name VideoPlayer -ErrorAction SilentlyContinue
    foreach ($w in $wins) {
        Write-Host "  PID=$($w.Id) Title='$($w.MainWindowTitle)' Handle=$($w.MainWindowHandle)"
    }

    # Don't kill, let user see
    Write-Host ""
    Write-Host "Leaving process running. Use Task Manager to close if needed."
}
