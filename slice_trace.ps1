# slice_trace.ps1 -- cut the start and the end out of a big SPURS trace so it
# can be sent over (each piece up to -Megabytes, cut at a line break).
#
#   powershell -ExecutionPolicy Bypass -File .\slice_trace.ps1
#   ... slice_trace.ps1 -Trace logs\spurs-20261021-174747.trace -Megabytes 350
#
# Without -Trace it takes the newest logs\spurs-*.trace. Writes
# <trace>.head.trace and <trace>.tail.trace beside it.

param(
    [string]$Trace = '',
    [int]$Megabytes = 350
)

. (Join-Path $PSScriptRoot 'common.ps1')

if (-not $Trace)
{
    $newest = Get-ChildItem -Path $LogDir -Filter 'spurs-*.trace' |
              Where-Object { $_.Name -notmatch '\.(head|tail)\.trace$' } |
              Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $newest)
    {
        throw 'no logs\spurs-*.trace found'
    }
    $Trace = $newest.FullName
}
$Trace = (Resolve-Path $Trace).Path
$sliceBytes = [int64]$Megabytes * 1MB

function Copy-Slice([System.IO.FileStream]$source, [int64]$start, [int64]$length, [string]$destination)
{
    # Copy [start, start + length) and stop at the last line break inside it.
    $output = [System.IO.File]::Create($destination)
    try
    {
        $buffer = New-Object byte[] (4MB)
        [void]$source.Seek($start, [System.IO.SeekOrigin]::Begin)
        $remaining = $length
        while ($remaining -gt 0)
        {
            $wanted = [int][math]::Min([int64]$buffer.Length, $remaining)
            $readCount = $source.Read($buffer, 0, $wanted)
            if ($readCount -le 0)
            {
                break
            }
            $writeCount = $readCount
            if ($remaining - $readCount -le 0)
            {
                $lastBreak = [array]::LastIndexOf($buffer, [byte]10, $readCount - 1)
                if ($lastBreak -ge 0)
                {
                    $writeCount = $lastBreak + 1
                }
            }
            $output.Write($buffer, 0, $writeCount)
            $remaining -= $readCount
        }
    }
    finally
    {
        $output.Close()
    }
}

$source = [System.IO.File]::OpenRead($Trace)
try
{
    $total = $source.Length
    $base = $Trace -replace '\.trace$', ''
    Write-Step "Head: first $Megabytes MB of $([math]::Round($total / 1GB, 2)) GB"
    Copy-Slice $source 0 $sliceBytes "$base.head.trace"
    if ($total -gt $sliceBytes)
    {
        Write-Step "Tail: last $Megabytes MB"
        # Start just after a line break so the tail begins on a whole line.
        $tailStart = [math]::Max([int64]0, $total - $sliceBytes)
        [void]$source.Seek($tailStart, [System.IO.SeekOrigin]::Begin)
        $probe = New-Object byte[] 65536
        $probeCount = $source.Read($probe, 0, $probe.Length)
        $firstBreak = [array]::IndexOf($probe, [byte]10, 0, $probeCount)
        if ($firstBreak -ge 0)
        {
            $tailStart += $firstBreak + 1
        }
        Copy-Slice $source $tailStart ($total - $tailStart) "$base.tail.trace"
    }
}
finally
{
    $source.Close()
}
Write-Step "Done: $base.head.trace, $base.tail.trace"
