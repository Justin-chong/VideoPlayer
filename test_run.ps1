# test_run.ps1 - 启动 VideoPlayer 并捕获所有输出和退出码
$buildDir = "F:\code\Qtcode\VideoPlayer-main\build\Debug"
$exe = Join-Path $buildDir "VideoPlayer.exe"
$stdout = Join-Path $buildDir "stdout.log"
$stderr = Join-Path $buildDir "stderr.log"
$exitFile = Join-Path $buildDir "exit_code.txt"

# 清理旧文件
Remove-Item $stdout, $stderr, $exitFile -ErrorAction SilentlyContinue

Write-Host "=== Launching: $exe ==="

# 启动进程（带超时）
$pinfo = New-Object System.Diagnostics.ProcessStartInfo
$pinfo.FileName = $exe
$pinfo.WorkingDirectory = $buildDir
$pinfo.RedirectStandardOutput = $true
$pinfo.RedirectStandardError = $true
$pinfo.UseShellExecute = $false
$pinfo.CreateNoWindow = $false

$p = [System.Diagnostics.Process]::Start($pinfo)

# 等待 3 秒（GUI 程序一般要时间初始化）
$exited = $p.WaitForExit(3000)

if ($exited)
{
    $code = $p.ExitCode
    Write-Host "Exited with code: $code"
    $out = $p.StandardOutput.ReadToEnd()
    $err = $p.StandardError.ReadToEnd()
    if ($out) { Write-Host "--- STDOUT ---"; Write-Host $out }
    if ($err) { Write-Host "--- STDERR ---"; Write-Host $err }
    $code | Out-File -FilePath $exitFile -Encoding ASCII
}
else
{
    Write-Host "Still running after 3s. Process ID: $($p.Id)"
    Write-Host "Window title: '$($p.MainWindowTitle)'"
    Write-Host "Has exited: $($p.HasExited)"

    # 检查是否弹窗
    $proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($proc) {
        Write-Host "Process: $($proc.ProcessName), StartTime: $($proc.StartTime)"
        Write-Host "Responding: $($proc.Responding)"
    }

    # 关闭
    Stop-Process -Id $p.Id -Force
    Write-Host "Killed process."
}
