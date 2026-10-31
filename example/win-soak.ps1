# =============================================================================
#  win-soak.ps1 —— Windows 侧的长时间运行监控
# =============================================================================
#  用法： powershell -ExecutionPolicy Bypass -File win-soak.ps1 [间隔秒=15]
#
#  每 N 秒采一次 rtsp_relay 的：
#    工作集（MB）、私有字节（MB）、句柄数、线程数、CPU 累计秒
#
#  输出：build-win/win-soak.csv
#
#  看趋势：
#    Import-Csv build-win/win-soak.csv | Format-Table -AutoSize
# =============================================================================
param([int]$Interval = 15)

$out = Join-Path $PSScriptRoot "..\build-win\win-soak.csv"
if (-not (Test-Path $out)) {
    "ts,iso,working_set_mb,private_mb,handles,threads,cpu_sec" |
        Out-File -FilePath $out -Encoding utf8
}

Write-Host "采样中（每 ${Interval}s）-> $out"
Write-Host "按 Ctrl-C 停止"

while ($true) {
    $p = Get-Process -Name rtsp_relay -ErrorAction SilentlyContinue
    if ($p) {
        $ws  = [math]::Round($p.WorkingSet64 / 1MB, 2)
        $pv  = [math]::Round($p.PrivateMemorySize64 / 1MB, 2)
        $ts  = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
        $iso = (Get-Date).ToString("o")
        "$ts,$iso,$ws,$pv,$($p.HandleCount),$($p.Threads.Count),$([math]::Round($p.TotalProcessorTime.TotalSeconds,1))" |
            Out-File -FilePath $out -Append -Encoding utf8
        Write-Host ("[{0}] 工作集 {1} MB, 私有 {2} MB, 句柄 {3}, 线程 {4}" -f `
                    (Get-Date -Format "HH:mm:ss"), $ws, $pv, $p.HandleCount, $p.Threads.Count)
    }
    Start-Sleep -Seconds $Interval
}
