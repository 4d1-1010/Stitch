# drive_windows.ps1 -- drive a Windows unio-pipe helper over its
# named pipe control socket. Mirror of drive_linux.py; same
# subcommand shape so scripts / docs reference one set of verbs.
#
# Usage (PowerShell):
#   .\drive_windows.ps1 <pipe-name> caps
#   .\drive_windows.ps1 <pipe-name> status
#   .\drive_windows.ps1 <pipe-name> in  <listen_port> <win_w> <win_h> <stream_id>
#   .\drive_windows.ps1 <pipe-name> out <peer_addr> <peer_port> <w> <h> <stream_id> [cx] [cy]
#   .\drive_windows.ps1 <pipe-name> stop <stream_id>
#   .\drive_windows.ps1 <pipe-name> idr  <stream_id>
#
# <pipe-name> is the bare name the helper was launched with
# (e.g. "unio-pipe-sink" for \\.\pipe\unio-pipe-sink -- the
# \\.\pipe\ prefix is added automatically by NamedPipeClientStream).
#
# Wire format: 4-byte little-endian length prefix + UTF-8 JSON
# body. Matches src/control_socket.cpp + drive_linux.py.

param(
    [Parameter(Mandatory=$true)][string]$PipeName,
    [Parameter(Mandatory=$true)][string]$Cmd,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$Rest
)

$ErrorActionPreference = 'Stop'

function Send-Rpc($pipe, [string]$json) {
    $b = [System.Text.Encoding]::UTF8.GetBytes($json)
    $pipe.Write([BitConverter]::GetBytes([uint32]$b.Length), 0, 4)
    $pipe.Write($b, 0, $b.Length)
    $pipe.Flush()
    $rl = New-Object byte[] 4
    $off = 0
    while ($off -lt 4) {
        $k = $pipe.Read($rl, $off, 4 - $off)
        if ($k -le 0) { throw "short read (length prefix)" }
        $off += $k
    }
    $n = [BitConverter]::ToUInt32($rl, 0)
    $r = New-Object byte[] $n
    $off = 0
    while ($off -lt $n) {
        $k = $pipe.Read($r, $off, $n - $off)
        if ($k -le 0) { throw "short read (body)" }
        $off += $k
    }
    [System.Text.Encoding]::UTF8.GetString($r, 0, $n)
}

$cli = New-Object System.IO.Pipes.NamedPipeClientStream '.', $PipeName, 'InOut'
$cli.Connect(5000)

try {
    switch ($Cmd) {
        'caps'   { Send-Rpc $cli '{"cmd":"helper_caps"}' }
        'status' { Send-Rpc $cli '{"cmd":"helper_status"}' }
        'in' {
            $port = [int]$Rest[0]; $w = [int]$Rest[1]; $h = [int]$Rest[2]; $sid = $Rest[3]
            Send-Rpc $cli "{`"cmd`":`"start_inbound`",`"stream_id`":`"$sid`",`"listen_port`":$port,`"window_w`":$w,`"window_h`":$h}"
        }
        'out' {
            $peer = $Rest[0]; $port = [int]$Rest[1]; $w = [int]$Rest[2]; $h = [int]$Rest[3]; $sid = $Rest[4]
            $cx = if ($Rest.Count -gt 5) { [int]$Rest[5] } else { 0 }
            $cy = if ($Rest.Count -gt 6) { [int]$Rest[6] } else { 0 }
            Send-Rpc $cli "{`"cmd`":`"start_outbound`",`"stream_id`":`"$sid`",`"peer_addr`":`"$peer`",`"peer_port`":$port,`"width`":$w,`"height`":$h,`"fps`":30,`"capture_x`":$cx,`"capture_y`":$cy,`"monitor_source`":`":1`"}"
        }
        'stop' {
            $sid = $Rest[0]
            Send-Rpc $cli "{`"cmd`":`"stop`",`"stream_id`":`"$sid`"}"
        }
        'idr' {
            $sid = $Rest[0]
            Send-Rpc $cli "{`"cmd`":`"request_idr`",`"stream_id`":`"$sid`"}"
        }
        default {
            Write-Error "unknown subcommand '$Cmd' -- see usage at top of this script"
        }
    }
} finally {
    $cli.Close()
}
