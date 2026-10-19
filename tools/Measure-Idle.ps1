[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [ValidateRange(180, 3600)][int]$Seconds = 180,
    [ValidateRange(1, 30)][int]$IntervalSeconds = 5,
    [ValidateRange(0, 120)][int]$WarmupSeconds = 15,
    [string]$Output
)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
function Read-Status {
    $text = & $exe status
    if ($LASTEXITCODE -ne 0) { throw "Resident status failed: $text" }
    $status = $text | ConvertFrom-Json
    if (!$status.ok -or $status.observeOnly -or $status.inputDesktop -ne 'Default' -or $status.hookInstalled) {
        throw 'Idle measurement requires the real resident on Default with no keyboard hook.'
    }
    return $status
}
$initial = Read-Status
$process = Get-Process -Id $initial.pid
if (![string]::Equals($process.Path, $exe, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Running PID does not match the supplied installed executable.'
}
$startTime = $process.StartTime
Start-Sleep -Seconds $WarmupSeconds
$before = Read-Status
$samples = [System.Collections.Generic.List[object]]::new()
$timer = [Diagnostics.Stopwatch]::StartNew()
$process.Refresh()
$cpuStart = $process.TotalProcessorTime.TotalSeconds
while ($true) {
    $process.Refresh()
    if ($process.HasExited -or $process.StartTime -ne $startTime) { throw 'Resident exited or PID was reused.' }
    $samples.Add([pscustomobject]@{
        elapsedSeconds = $timer.Elapsed.TotalSeconds
        workingSetBytes = $process.WorkingSet64
        privateBytes = $process.PrivateMemorySize64
        threads = $process.Threads.Count
        handles = $process.HandleCount
    })
    if ($timer.Elapsed.TotalSeconds -ge $Seconds) { break }
    Start-Sleep -Milliseconds ([int](1000 * [Math]::Min($IntervalSeconds, $Seconds - $timer.Elapsed.TotalSeconds)))
}
$elapsed = $timer.Elapsed.TotalSeconds
$cpuEnd = $process.TotalProcessorTime.TotalSeconds
$after = Read-Status
if ($after.pid -ne $initial.pid -or $after.foreignDesktopObservations -ne $before.foreignDesktopObservations -or
    $after.inaccessibleDesktopObservations -ne $before.inaccessibleDesktopObservations) {
    throw 'Desktop state changed during the idle measurement; do not label this sample baseline-idle.'
}
$logicalProcessors = [Environment]::ProcessorCount
$cpuSeconds = $cpuEnd - $cpuStart
function Summarize($property) {
    $stats = $samples | Measure-Object -Property $property -Average -Minimum -Maximum
    return [ordered]@{
        average = $stats.Average
        minimum = $stats.Minimum
        maximum = $stats.Maximum
        first = $samples[0].$property
        final = $samples[$samples.Count - 1].$property
    }
}
$result = [ordered]@{
    executable = $exe
    sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLower()
    pid = $initial.pid
    osVersion = [Environment]::OSVersion.Version.ToString()
    osArchitecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    measurementUtc = [DateTime]::UtcNow.ToString('o')
    elapsedSeconds = $elapsed
    warmupSeconds = $WarmupSeconds
    sampleCount = $samples.Count
    logicalProcessors = $logicalProcessors
    cpuStartSeconds = $cpuStart
    cpuEndSeconds = $cpuEnd
    cpuDeltaSeconds = $cpuSeconds
    cpuPercentOneCore = 100 * $cpuSeconds / $elapsed
    cpuPercentMachine = 100 * $cpuSeconds / $elapsed / $logicalProcessors
    workingSetBytes = (Summarize 'workingSetBytes')
    privateBytes = (Summarize 'privateBytes')
    threadCount = (Summarize 'threads')
    handleCount = (Summarize 'handles')
    processCount = 1
    baselineDesktop = $after.baselineDesktop
    hookInstalled = $after.hookInstalled
    pollMilliseconds = $after.fallbackPollMilliseconds
    foreignDesktopObservationsDuringMeasurement = $after.foreignDesktopObservations - $before.foreignDesktopObservations
    samples = $samples
}
$json = $result | ConvertTo-Json -Depth 8
if ($Output) { Set-Content -LiteralPath $Output -Value $json -Encoding utf8 }
$result.Remove('samples')
$result | ConvertTo-Json -Depth 8
