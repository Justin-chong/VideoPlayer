# test_run4.ps1 - Launch and wait longer for window
$buildDir = "F:\code\Qtcode\VideoPlayer-main\build\Debug"
$exe = Join-Path $buildDir "VideoPlayer.exe"

Write-Host "=== Launching: $exe ==="

# Launch
$proc = Start-Process -FilePath $exe -WorkingDirectory $buildDir -PassThru

# Wait up to 10 seconds for window
$found = $false
for ($i = 0; $i -lt 10; $i++)
{
    Start-Sleep -Seconds 1
    $proc.Refresh()
    if (-not $proc.HasExited)
    {
        if ($proc.MainWindowHandle -ne 0)
        {
            $found = $true
            Write-Host "Window appeared after $i seconds"
            break
        }
    }
    else
    {
        Write-Host "Process exited at $i seconds with code $($proc.ExitCode)"
        break
    }
}

if (-not $proc.HasExited)
{
    $proc.Refresh()
    Write-Host "Final state:"
    Write-Host "  PID: $($proc.Id)"
    Write-Host "  Window title: '$($proc.MainWindowTitle)'"
    Write-Host "  Window handle: $($proc.MainWindowHandle)"
    Write-Host "  Responding: $($proc.Responding)"
    Write-Host "  Threads: $($proc.Threads.Count)"
    Write-Host "  Working set: $([math]::Round($proc.WorkingSet64/1MB, 2)) MB"
    Write-Host "  Handles: $($proc.HandleCount)"
    Write-Host "  Start time: $($proc.StartTime)"

    # Don't kill; let user close manually
    Write-Host ""
    Write-Host "Process is still running. Close it via Task Manager or by clicking X."
}
else
{
    Write-Host "Process already exited with code: $($proc.ExitCode)"
}
