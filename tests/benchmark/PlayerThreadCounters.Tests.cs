using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

public static class PlayerThreadCounterTests {
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentThread();
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern int SetThreadDescription(IntPtr thread, string description);
    [DllImport("kernel32.dll")] static extern bool GetProcessHandleCount(IntPtr process, out uint count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateWindowEx(uint exStyle, string cls, string title, uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    static void Check(bool ok, string reason) { if (!ok) throw new Exception(reason); }
    static uint Handles() { uint count; Check(GetProcessHandleCount(new IntPtr(-1), out count), "GetProcessHandleCount"); return count; }

    public static string[] Run() {
        var checks = new List<string>();
        int pid = Process.GetCurrentProcess().Id;
        var first = PlayerThreadCounters.Capture(pid);
        Check(first.unavailableReason == null && first.threads.Length > 0, "Own process unavailable");
        Check(first.qpcEnd >= first.qpcStart && first.qpcFrequency > 0, "Invalid collection interval");
        Check(first.threads.Select(t => t.id).Distinct().Count() == first.threads.Length, "Duplicate thread IDs");
        checks.Add("owned process, unique threads and monotonic capture bounds");
        var invalid = PlayerThreadCounters.Capture(int.MaxValue);
        Check(invalid.unavailableReason != null && invalid.threads.Length == 0, "Missing process reported as a valid zero sample");
        checks.Add("missing process has an explicit unavailable reason");

        uint workerId = 0; int stop = 0; int named = -1;
        using (var ready = new ManualResetEvent(false))
        using (var start = new ManualResetEvent(false))
        using (var finished = new ManualResetEvent(false))
        using (var release = new ManualResetEvent(false)) {
            var worker = new Thread(() => {
                workerId = GetCurrentThreadId(); named = SetThreadDescription(GetCurrentThread(), "Aperture counter fixture");
                ready.Set(); start.WaitOne();
                while (Volatile.Read(ref stop) == 0) Thread.SpinWait(100);
                finished.Set(); release.WaitOne();
            });
            worker.IsBackground = true; worker.Start();
            try {
                Check(ready.WaitOne(5000) && named >= 0, "Named worker not ready");
                var before = PlayerThreadCounters.Capture(pid).threads.Single(t => t.id == workerId);
                start.Set(); Thread.Sleep(150); Volatile.Write(ref stop, 1);
                Check(finished.WaitOne(5000), "Worker stopped making progress");
                var after = PlayerThreadCounters.Capture(pid).threads.Single(t => t.id == workerId);
                Check(before.createdFileTimeUtc != null && before.createdFileTimeUtc == after.createdFileTimeUtc, "Lost thread creation identity");
                Check(before.cpuCycles.HasValue && after.cpuCycles > before.cpuCycles && after.qpc > before.qpc, "Busy worker cycles did not increase");
                checks.Add("busy worker retains creation identity and increasing cycles");
                Check(after.description == "Aperture counter fixture", "Thread description missing");
                checks.Add("native thread descriptions survive capture");
                PlayerThreadCounters.Capture(pid); uint handles = Handles();
                for (int i=0; i<20; ++i) PlayerThreadCounters.Capture(pid);
                Check(Handles() <= handles + 4, "Repeated capture leaked thread handles");
                checks.Add("repeated snapshots close native thread handles");
            } finally {
                Volatile.Write(ref stop, 1); start.Set(); release.Set(); worker.Join(5000);
            }
        }
        IntPtr window = CreateWindowEx(0, "STATIC", "counter fixture", 0x80000000, 0, 0, 32, 32, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        Check(window != IntPtr.Zero, "Hidden fixture window unavailable");
        try {
            Check(!IsWindowVisible(window), "Fixture must remain hidden");
            Check(PlayerBenchmarkWindow.OwnerThread(window, (uint)pid) == GetCurrentThreadId(), "Wrong UI thread");
            Check(PlayerBenchmarkWindow.OwnerThread(window, (uint)pid+1) == 0, "Accepted another window owner");
            checks.Add("hidden owner-window thread lookup checks process identity");
        } finally { DestroyWindow(window); }
        return checks.ToArray();
    }
}
