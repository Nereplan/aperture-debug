using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Text;
public static class PlayerBenchmarkWindow {
    public delegate bool EnumProc(IntPtr window, IntPtr context);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr context);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr window, EnumProc callback, IntPtr context);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder name, int length);
    [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("kernel32.dll", EntryPoint="QueryProcessCycleTime", SetLastError=true)]
    public static extern bool QueryProcessCycles(IntPtr process, out ulong cycles);
    public struct Rect { public int left, top, right, bottom; }
    public sealed class ChildWindow {
        public long handle;
        public string className;
        public int clientWidth, clientHeight;
        public uint dpi;
    }
    public static ChildWindow[] Children(IntPtr root, uint pid) {
        uint rootOwner; GetWindowThreadProcessId(root, out rootOwner);
        if (rootOwner != pid) throw new ArgumentException("Root window does not belong to the benchmark player.");
        var result = new List<ChildWindow>();
        EnumChildWindows(root, (window, context) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            Rect rect;
            if (owner != pid || !IsWindowVisible(window) || !GetClientRect(window, out rect)) return true;
            var name = new StringBuilder(256); GetClassName(window, name, name.Capacity);
            result.Add(new ChildWindow { handle=window.ToInt64(), className=name.ToString(),
                clientWidth=rect.right-rect.left, clientHeight=rect.bottom-rect.top, dpi=GetDpiForWindow(window) });
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
    public static ChildWindow VideoCanvas(IntPtr root, uint pid, string player) {
        foreach (var child in Children(root, pid)) {
            if ((player == "Aperture" && child.className == "mpv") ||
                (player == "VLC" && child.className.StartsWith("VLC video main ", StringComparison.Ordinal))) return child;
        }
        return null;
    }
    public static bool MatchVideoCanvas(IntPtr root, uint pid, string player, int width, int height) {
        if (width < 320 || width > 3840 || height < 180 || height > 2160) return false;
        for (int attempt=0; attempt < 20; ++attempt) {
            var canvas = VideoCanvas(root, pid, player);
            if (canvas == null) return false;
            if (canvas.clientWidth == width && canvas.clientHeight == height) return true;
            var outer = Size(root);
            if (outer == null || !Resize(root, pid, outer[0]+width-canvas.clientWidth, outer[1]+height-canvas.clientHeight)) return false;
            System.Threading.Thread.Sleep(100);
        }
        return false;
    }
    public static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, context) => { uint owner; GetWindowThreadProcessId(window, out owner);
            if(owner == pid && IsWindowVisible(window) && GetWindow(window, 4) == IntPtr.Zero) { found = window; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    public static bool Resize(IntPtr window, uint pid, int width, int height) { uint owner; GetWindowThreadProcessId(window, out owner); return owner == pid && SetWindowPos(window, IntPtr.Zero, 40, 40, width, height, 0x0014); }
    public static uint OwnerThread(IntPtr window, uint pid) { uint owner; uint thread = GetWindowThreadProcessId(window, out owner); return owner == pid ? thread : 0; }
    public static int[] Size(IntPtr window) { Rect rect; if(!GetWindowRect(window, out rect)) return null; return new[]{rect.right-rect.left, rect.bottom-rect.top}; }
    public static void Close(IntPtr window, uint pid) { uint owner; GetWindowThreadProcessId(window, out owner); if(owner == pid) PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero); }
}
