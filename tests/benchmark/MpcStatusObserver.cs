// Read-only MPC-HC /slave observer. Protocol: official 2.8.3 MpcApi.h.
// Its hidden message window lives on a dedicated thread so synchronous
// WM_COPYDATA replies can be serviced while PowerShell samples the process.
using System;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Threading;

public sealed class MpcStatusObserver : IDisposable {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WindowClass {
        public uint style; public WindowProc callback; public int clsExtra, wndExtra;
        public IntPtr instance, icon, cursor, background;
        public string menu, name;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct Message {
        public IntPtr window; public uint message; public UIntPtr w; public IntPtr l;
        public uint time; public int x, y; public uint privateData;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct CopyData { public UIntPtr command; public int bytes; public IntPtr text; }
    delegate IntPtr WindowProc(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern ushort RegisterClassW(ref WindowClass cls);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool UnregisterClassW(string name, IntPtr instance);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateWindowExW(uint ex, string cls, string title, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr DefWindowProcW(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern int GetMessageW(out Message message, IntPtr window, uint low, uint high);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessageW(ref Message message);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeoutW(IntPtr window, uint message, IntPtr w, ref CopyData data, uint flags, uint timeout, out UIntPtr result);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string name);

    readonly Thread thread;
    readonly ManualResetEventSlim ready = new ManualResetEventSlim();
    readonly object sync = new object();
    readonly WindowProc callback;
    IntPtr host, player;
    uint ownedPid;
    double? position;
    Exception startupError;
    public long Handle { get { return host.ToInt64(); } }
    public MpcStatusObserver() {
        callback = Receive;
        thread = new Thread(Loop) { IsBackground = true };
        thread.Start();
        if (!ready.Wait(3000)) throw new TimeoutException("MPC observer startup timed out.");
        if (startupError != null) throw startupError;
    }
    void Loop() {
        string name = "Aperture_Mpc_Benchmark_" + Guid.NewGuid().ToString("N");
        IntPtr instance = GetModuleHandleW(null);
        try {
            var cls = new WindowClass { callback = callback, instance = instance, name = name };
            if (RegisterClassW(ref cls) == 0) throw new InvalidOperationException("Cannot register MPC observer.");
            host = CreateWindowExW(0, name, "", 0, 0, 0, 0, 0, new IntPtr(-3), IntPtr.Zero, instance, IntPtr.Zero);
            if (host == IntPtr.Zero) throw new InvalidOperationException("Cannot create MPC observer.");
            ready.Set();
            Message message;
            while (GetMessageW(out message, IntPtr.Zero, 0, 0) > 0) {
                if (message.message == 0x8001) break;
                DispatchMessageW(ref message);
            }
        } catch (Exception error) { startupError = error; ready.Set(); }
        finally { if (host != IntPtr.Zero) DestroyWindow(host); UnregisterClassW(name, instance); }
    }
    IntPtr Receive(IntPtr window, uint message, IntPtr w, IntPtr l) {
        if (message != 0x004A || l == IntPtr.Zero) return DefWindowProcW(window, message, w, l);
        // The handshake can precede SetOwnedPid. Retain only its window, then
        // authenticate both that window and every later reply before querying.
        var data = Marshal.PtrToStructure<CopyData>(l);
        if (data.text == IntPtr.Zero || data.bytes < 2 || data.bytes > 128 || data.bytes % 2 != 0) return IntPtr.Zero;
        string value = Marshal.PtrToStringUni(data.text, data.bytes / 2).TrimEnd('\0');
        lock (sync) {
            if (data.command.ToUInt64() == 0x50000000) {
                long handle;
                if (long.TryParse(value, out handle) && new IntPtr(handle) == w) player = w;
            } else if (data.command.ToUInt64() == 0x50000007 && ownedPid != 0 && w == player) {
                uint pid; GetWindowThreadProcessId(w, out pid);
                double seconds;
                if (pid == ownedPid && double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out seconds) && !double.IsNaN(seconds) && !double.IsInfinity(seconds)) position = seconds;
            }
        }
        return new IntPtr(1);
    }
    public void SetOwnedPid(uint pid) { lock (sync) { ownedPid = pid; } }
    public double Query(uint pid) {
        IntPtr target;
        lock (sync) { target = player; position = null; if (ownedPid != pid) throw new InvalidOperationException("MPC PID changed."); }
        uint actual; GetWindowThreadProcessId(target, out actual);
        if (target == IntPtr.Zero || actual != pid) throw new InvalidOperationException("MPC handshake missing or wrong PID.");
        IntPtr text = Marshal.StringToHGlobalUni("");
        try {
            var data = new CopyData { command = new UIntPtr(0xA0003004), bytes = 2, text = text };
            UIntPtr result;
            if (SendMessageTimeoutW(target, 0x004A, host, ref data, 2, 1500, out result) == IntPtr.Zero) throw new TimeoutException("MPC position request timed out.");
            lock (sync) { if (position.HasValue) return position.Value; }
            throw new InvalidOperationException("MPC returned no authenticated position.");
        } finally { Marshal.FreeHGlobal(text); }
    }
    public void Dispose() {
        if (host != IntPtr.Zero) PostMessageW(host, 0x8001, IntPtr.Zero, IntPtr.Zero);
        thread.Join(2000);
        ready.Dispose();
    }
}
