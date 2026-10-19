using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;

// Passive, cumulative counters only: never suspend a player or walk its stacks.
public static class PlayerThreadCounters {
    [StructLayout(LayoutKind.Sequential)] struct FileTime { public uint low, high; }
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint GetProcessIdOfThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool QueryThreadCycleTime(IntPtr thread, out ulong cycles);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetThreadTimes(IntPtr thread, out FileTime created, out FileTime exited, out FileTime kernel, out FileTime user);
    [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr thread, out IntPtr description);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    static ulong Value(FileTime value) { return ((ulong)value.high << 32) | value.low; }

    public sealed class Counter {
        public uint id;
        // File times exceed JavaScript's exact integer range. Keep identity lossless.
        public string createdFileTimeUtc;
        public ulong? cpuCycles, kernelCpu100ns, userCpu100ns;
        public long qpc;
        public string description, unavailableReason, descriptionUnavailableReason;
    }
    public sealed class Snapshot {
        public int pid;
        public long qpcStart, qpcEnd, qpcFrequency;
        public Counter[] threads = new Counter[0];
        public string unavailableReason;
    }
    public static Snapshot Capture(int pid) {
        var snapshot = new Snapshot { pid=pid, qpcStart=Stopwatch.GetTimestamp(), qpcFrequency=Stopwatch.Frequency };
        var counters = new List<Counter>();
        try {
            using (var process = Process.GetProcessById(pid)) {
                foreach (ProcessThread discovered in process.Threads) {
                    using (discovered) {
                        var counter = new Counter { id=(uint)discovered.Id };
                        counters.Add(counter);
                        IntPtr handle = OpenThread(0x0800, false, counter.id); // QUERY_LIMITED_INFORMATION
                        if (handle == IntPtr.Zero) {
                            counter.unavailableReason = "OpenThread: " + Marshal.GetLastWin32Error();
                            continue;
                        }
                        try {
                            // A thread ID can be reused between enumeration and opening.
                            if (GetProcessIdOfThread(handle) != (uint)pid) {
                                counter.unavailableReason = "Thread owner changed or became unavailable";
                                continue;
                            }
                            FileTime created, exited, kernel, user;
                            if (GetThreadTimes(handle, out created, out exited, out kernel, out user)) {
                                counter.createdFileTimeUtc = Value(created).ToString(CultureInfo.InvariantCulture);
                                counter.kernelCpu100ns = Value(kernel); counter.userCpu100ns = Value(user);
                            } else {
                                counter.unavailableReason = "GetThreadTimes: " + Marshal.GetLastWin32Error();
                                continue;
                            }
                            ulong cycles;
                            counter.qpc = Stopwatch.GetTimestamp();
                            if (QueryThreadCycleTime(handle, out cycles)) counter.cpuCycles = cycles;
                            else counter.unavailableReason = "QueryThreadCycleTime: " + Marshal.GetLastWin32Error();
                            IntPtr description = IntPtr.Zero;
                            try {
                                int result = GetThreadDescription(handle, out description);
                                if (result >= 0) counter.description = Marshal.PtrToStringUni(description) ?? "";
                                else counter.descriptionUnavailableReason = "GetThreadDescription: " + result;
                            } catch (EntryPointNotFoundException) {
                                counter.descriptionUnavailableReason = "GetThreadDescription unavailable on this Windows version";
                            } finally { if (description != IntPtr.Zero) LocalFree(description); }
                        } finally { CloseHandle(handle); }
                    }
                }
            }
        } catch (Exception error) {
            snapshot.unavailableReason = error.GetType().Name + ": " + error.Message;
        } finally {
            snapshot.threads = counters.ToArray(); snapshot.qpcEnd = Stopwatch.GetTimestamp();
        }
        return snapshot;
    }
}
