# Stable input identity, collected before any timed player work.
function Get-PlayerRuntimeManifest {
    param([Parameter(Mandatory)][string]$Root)
    $resolved = (Get-Item -LiteralPath $Root -Force -ErrorAction Stop).FullName.TrimEnd('\','/')
    $items = @(Get-Item -LiteralPath $resolved -Force) + @(Get-ChildItem -LiteralPath $resolved -Recurse -Force -ErrorAction Stop)
    if (@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
        throw 'Linked runtime inputs cannot establish a stable player identity.'
    }
    $ordered = [Collections.Generic.SortedDictionary[string,object]]::new([StringComparer]::Ordinal)
    foreach ($file in $items | Where-Object { -not $_.PSIsContainer }) {
        $relative = $file.FullName.Substring($resolved.Length + 1).Replace('\','/')
        $ordered.Add($relative, [ordered]@{
            path = $relative
            bytes = [long]$file.Length
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        })
    }
    $canonical = [Text.StringBuilder]::new()
    $total = [long]0
    foreach ($file in $ordered.Values) {
        [void]$canonical.Append($file.path).Append("`t").Append($file.bytes).Append("`t").Append($file.sha256).Append("`n")
        $total += $file.bytes
    }
    $hash = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($canonical.ToString()))).Replace('-','').ToLowerInvariant() }
    finally { $hash.Dispose() }
    [pscustomobject]@{sha256=$digest;bytes=$total;files=@($ordered.Values)}
}

function Get-PlayerBenchmarkEnvironment {
    $system = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    $cpu = @(Get-CimInstance Win32_Processor -ErrorAction Stop)
    $gpu = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | Sort-Object PNPDeviceID)
    if ($system.NumberOfLogicalProcessors -lt 1 -or -not $cpu.Count -or -not $gpu.Count) {
        throw 'Cannot verify the benchmark processor/GPU environment.'
    }
    [ordered]@{
        osVersion = "$($os.Caption) $($os.Version) build $($os.BuildNumber)"
        cpuName = ($cpu.Name -join ' | ')
        logicalProcessors = [int]$system.NumberOfLogicalProcessors
        gpuName = ($gpu.Name -join ' | ')
        gpuDriverVersion = ($gpu.DriverVersion -join ' | ')
        lastBootUpTimeUtc = $os.LastBootUpTime.ToUniversalTime().ToString('o')
        source = 'Win32_ComputerSystem, Win32_OperatingSystem, Win32_Processor and Win32_VideoController CIM instances'
    }
}
