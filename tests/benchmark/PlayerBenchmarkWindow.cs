using System;
using System.Runtime.InteropServices;
public static class PlayerBenchmarkWindow {
    public delegate bool EnumProc(IntPtr window, IntPtr context);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr context);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("kernel32.dll", EntryPoint="QueryProcessCycleTime", SetLastError=true)]
    public static extern bool QueryProcessCycles(IntPtr process, out ulong cycles);
    public struct Rect { public int left, top, right, bottom; }
    public static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, context) => { uint owner; GetWindowThreadProcessId(window, out owner);
            if(owner == pid && IsWindowVisible(window) && GetWindow(window, 4) == IntPtr.Zero) { found = window; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    public static bool Resize(IntPtr window, uint pid, int width, int height) { uint owner; GetWindowThreadProcessId(window, out owner); return owner == pid && SetWindowPos(window, IntPtr.Zero, 40, 40, width, height, 0x0014); }
    public static int[] Size(IntPtr window) { Rect rect; if(!GetWindowRect(window, out rect)) return null; return new[]{rect.right-rect.left, rect.bottom-rect.top}; }
    public static void Close(IntPtr window, uint pid) { uint owner; GetWindowThreadProcessId(window, out owner); if(owner == pid) PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero); }
}
