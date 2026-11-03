param(
    [int]$Runs = 3,
    [int]$Rounds = 256,
    [int]$Work = 128,
    [string[]]$Compilers = @('g++', 'clang++'),
    [string]$CaseFilter = '*',
    [switch]$ControlAdapter,
    [switch]$Smoke,
    [string]$OutputDirectory = (Join-Path $env:TEMP 'segmented_flags_compile_bench')
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = Join-Path $PSScriptRoot 'segmented_flags_compile_bench.cpp'
if ($ControlAdapter) { $source = Join-Path $PSScriptRoot 'bank_control_compile_bench.cpp' }
if ($Smoke) { $source = Join-Path $root 'examples/test_smoke.cpp' }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null

if (-not ('SegmentedBenchmarkMemory' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SegmentedBenchmarkMemory {
    [StructLayout(LayoutKind.Sequential)]
    private struct Counters {
        public uint Size, PageFaultCount;
        public UIntPtr PeakWorkingSetSize, WorkingSetSize;
        public UIntPtr QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
        public UIntPtr QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
        public UIntPtr PagefileUsage, PeakPagefileUsage;
    }
    [DllImport("psapi.dll", SetLastError = true)]
    private static extern bool GetProcessMemoryInfo(IntPtr process, ref Counters counters, uint size);
    public static double PeakMiB(IntPtr process) {
        var counters = new Counters();
        counters.Size = (uint)Marshal.SizeOf<Counters>();
        if (!GetProcessMemoryInfo(process, ref counters, counters.Size))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        return counters.PeakWorkingSetSize.ToUInt64() / 1048576.0;
    }
}
'@
}

$cases = @(
    @{ Name = 'parse'; Kind = 0; Segments = 2; Work = 0; Switch = 0; Rounds = 0 },
    @{ Name = 'u64-light'; Kind = 0; Segments = 2; Work = 0; Switch = 0 },
    @{ Name = 'u128-member-light'; Kind = 4; Segments = 2; Work = 0; Switch = 0 },
    @{ Name = 'u64-heavy'; Kind = 0; Segments = 2; Work = $Work; Switch = 0 },
    @{ Name = 'u128-operators-heavy'; Kind = 1; Segments = 2; Work = $Work; Switch = 0 },
    @{ Name = 'u128-member-heavy'; Kind = 4; Segments = 2; Work = $Work; Switch = 0 }
)
foreach ($segments in 2, 4, 16, 64) {
    foreach ($kind in 2, 3) {
        $method = if ($kind -eq 2) { 'direct' } else { 'window' }
        $cases += @{ Name = "bank$segments-$method-light"; Kind = $kind; Segments = $segments; Work = 0; Switch = 0 }
        $cases += @{ Name = "bank$segments-$method-heavy"; Kind = $kind; Segments = $segments; Work = $Work; Switch = 0 }
    }
}
foreach ($switch in 1, 2, 3) {
    foreach ($kind in 2, 3) {
        $method = if ($kind -eq 2) { 'direct' } else { 'window' }
        $cases += @{ Name = "bank16-$method-switch$switch"; Kind = $kind; Segments = 16; Work = $Work; Switch = $switch }
    }
}

if ($ControlAdapter) {
    $cases = @()
    foreach ($legacy in 1, 0) {
        $method = if ($legacy) { 'legacy' } else { 'native' }
        $cases += @{ Name = "control-u128-$method"; Kind = 4; Segments = 2; Work = 0; Switch = 0; Legacy = $legacy }
        foreach ($segments in 2, 16, 64) {
            $cases += @{ Name = "control-bank$segments-$method"; Kind = 2; Segments = $segments; Work = 0; Switch = 0; Legacy = $legacy }
        }
    }
}
if ($Smoke) {
    $cases = @()
    foreach ($kind in 0, 1, 2, 3) {
        $name = @('iterator', 'exp-list', 'inert-list', 'aligned')[$kind]
        $cases += @{ Name = "smoke-$name"; Kind = $kind; Segments = 2; Work = 0; Switch = 0 }
    }
}

$samples = [System.Collections.Generic.List[object]]::new()
foreach ($compiler in $Compilers) {
    $executable = (Get-Command $compiler).Source
    $prefix = $null
    if ($compiler -like '*clang*') {
        $trace = (& $executable '-###' -std=c++23 -fsyntax-only $source 2>&1 | Out-String)
        if ($trace -notmatch '"([^"]+\.exe)" "-cc1"') {
            throw 'Cannot locate the actual Clang frontend executable'
        }
        $executable = $Matches[1]
    } else {
        $prefix = (Split-Path (& $executable -print-libgcc-file-name)) + '/'
        $executable = (& $executable -print-prog-name=cc1plus).Trim()
    }
    foreach ($case in $cases) {
        if ($case.Name -notlike $CaseFilter) { continue }
        for ($run = 0; $run -lt $Runs; ++$run) {
            $count = if ($case.ContainsKey('Rounds')) { $case.Rounds } else { $Rounds }
            $start = [System.Diagnostics.ProcessStartInfo]::new($executable)
            $start.UseShellExecute = $false
            $start.RedirectStandardOutput = $true
            $start.RedirectStandardError = $true
            $start.WorkingDirectory = $root
            $legacy = if ($case.ContainsKey('Legacy')) { $case.Legacy } else { 0 }
            foreach ($argument in @('-std=c++23', '-fsyntax-only', '-I', $root,
                "-DBENCH_KIND=$($case.Kind)", "-DBENCH_SEGMENTS=$($case.Segments)",
                "-DBENCH_WORK=$($case.Work)", "-DBENCH_SWITCH=$($case.Switch)",
                "-DBENCH_ROUNDS=$count", "-DBENCH_LEGACY=$legacy", '-ftemplate-depth=4096', $source)) {
                $start.ArgumentList.Add($argument)
            }
            if ($compiler -like '*clang*') {
                $start.ArgumentList.Add('--driver-mode=g++')
                $start.ArgumentList.Add('--target=x86_64-w64-windows-gnu')
                $start.ArgumentList.Add('-fconstexpr-steps=100000000')
                $start.ArgumentList.Add('-fintegrated-cc1')
            } else {
                $start.ArgumentList.Add('-quiet')
                $start.ArgumentList.Add('-iprefix')
                $start.ArgumentList.Add($prefix)
                $start.ArgumentList.Add('-D_REENTRANT')
                $start.ArgumentList.Add('-o')
                $start.ArgumentList.Add('nul')
                $start.ArgumentList.Add('-fconstexpr-ops-limit=1000000000')
                $start.ArgumentList.Add('-ftime-report')
            }
            $timer = [System.Diagnostics.Stopwatch]::StartNew()
            $process = [System.Diagnostics.Process]::Start($start)
            $handle = $process.Handle
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            $process.WaitForExit()
            $timer.Stop()
            $errorText = $stderr.GetAwaiter().GetResult()
            $null = $stdout.GetAwaiter().GetResult()
            $peak = [SegmentedBenchmarkMemory]::PeakMiB($handle)
            $exitCode = $process.ExitCode
            $process.Dispose()
            if ($exitCode -ne 0 -or $errorText -match 'warning:') {
                throw "$compiler $($case.Name) invalid run: $errorText"
            }
            $gcMiB = $null
            if ($errorText -match 'TOTAL\s*:[^\r\n]*?\s+(\d+)M\s*(?:\r?\n|$)') {
                $gcMiB = [int]$Matches[1]
            }
            $samples.Add([pscustomobject]@{
                Compiler = $compiler; Case = $case.Name; Rounds = $count;
                Work = $case.Work; Segments = $case.Segments; Switch = $case.Switch;
                Run = $run + 1; Ms = [math]::Round($timer.Elapsed.TotalMilliseconds, 1);
                FrontendPeakMiB = [math]::Round($peak, 1); GccAllocatedMiB = $gcMiB
            })
        }
        $samples | Export-Csv (Join-Path $OutputDirectory 'samples.csv') -NoTypeInformation
        $group = @($samples | Where-Object { $_.Compiler -eq $compiler -and $_.Case -eq $case.Name } | Sort-Object Ms)
        $median = $group[[int][math]::Floor($group.Count / 2)]
        Write-Host "$compiler $($case.Name): median=$($median.Ms) ms, frontend peak=$($median.FrontendPeakMiB) MiB, GCC allocated=$($median.GccAllocatedMiB) MiB"
    }
}

$summary = $samples | Group-Object Compiler, Case | ForEach-Object {
    $ordered = @($_.Group | Sort-Object Ms)
    $median = $ordered[[int][math]::Floor($ordered.Count / 2)]
    [pscustomobject]@{
        Compiler = $median.Compiler; Case = $median.Case; Rounds = $median.Rounds;
        Work = $median.Work; MedianMs = $median.Ms; MinMs = $ordered[0].Ms;
        FrontendPeakMiB = $median.FrontendPeakMiB; GccAllocatedMiB = $median.GccAllocatedMiB
    }
}
$summary | Export-Csv (Join-Path $OutputDirectory 'summary.csv') -NoTypeInformation
$summary | Format-Table -AutoSize