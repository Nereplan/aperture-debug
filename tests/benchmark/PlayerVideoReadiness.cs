using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// Sparse desktop-GDI readback of a controlled fixture, never PrintWindow/back buffers.
// Recognition is visible sentinel evidence, not photon timing, frame pacing or audio proof.
public static class PlayerVideoReadiness {
    public struct Rect { public int left, top, right, bottom; }
    struct Point { public int x, y; public Point(int x, int y) { this.x=x; this.y=y; } }
    [StructLayout(LayoutKind.Sequential)] struct MonitorInfo { public int size; public Rect monitor, work; public uint flags; }
    [StructLayout(LayoutKind.Sequential)] struct CursorInfo { public int size; public uint flags; public IntPtr cursor; public Point position; }
    [StructLayout(LayoutKind.Sequential)] struct IconInfo { public bool icon; public uint xHotspot, yHotspot; public IntPtr mask, color; }
    [StructLayout(LayoutKind.Sequential)] struct Bitmap { public int type, width, height, widthBytes; public ushort planes, bitsPixel; public IntPtr bits; }
    delegate bool EnumProc(IntPtr window, IntPtr context);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr context);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr root, EnumProc callback, IntPtr context);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] static extern bool IsChild(IntPtr parent, IntPtr child);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
    [DllImport("user32.dll")] static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr window, uint attribute, out uint value, int size);
    [DllImport("user32.dll")] static extern bool GetCursorInfo(ref CursorInfo info);
    [DllImport("user32.dll")] static extern bool GetIconInfo(IntPtr icon, out IconInfo info);
    [DllImport("gdi32.dll", EntryPoint="GetObjectW")] static extern int GetObject(IntPtr obj, int size, out Bitmap bitmap);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] static extern uint GetPixel(IntPtr dc, int x, int y);

    static readonly int[] palette = { 0xe62828, 0x28d73c, 0x3246e6, 0x1ed2dc, 0xdc28d2, 0xe6d21e };
    public static int[] Palette { get { return (int[])palette.Clone(); } }
    public const int PhaseCount = 4, PhaseSeconds = 12, ColorTolerance = 40;
    public sealed class Sample {
        public string status, reason;
        public bool safe;
        public int phase = -1;
        public long qpcStart, qpcReadStart, qpcReadEnd, qpcEnd, root, canvas;
        public uint dpi;
        public Rect screenCanvas;
        public int[] rgb = new int[0];
    }
    public sealed class Result {
        public string status, reason, processStartedUtc;
        public uint pid;
        public int expectedPhase, pollMilliseconds, capturedSamples, pendingPolls, nonmatchingSamples;
        public long originQpc, observerStartedQpc, qpcFrequency = Stopwatch.Frequency;
        public double? lowerBoundMs, upperBoundMs, confirmationMs;
        public bool leftCensored;
        public Sample lastNegative, firstPositive, confirmation;
        public string method = "Desktop GDI RGB at 54 owned sentinel points, with visibility/occlusion checks before and after readback; two consecutive matches. Bounds include observer/control latency. Not compositor frame IDs, physical display/audio output or pacing.";
    }

    public static int[] ExpectedRgb(int phase) {
        if (phase < 0 || phase >= PhaseCount) throw new ArgumentOutOfRangeException("phase");
        var values = new int[54];
        for (int i=0; i<values.Length; ++i) values[i]=palette[(i/9+phase)%palette.Length];
        return values;
    }
    public static int RecognizeRgb(int[] rgb) {
        if (rgb == null || rgb.Length != 54) throw new ArgumentException("Exactly 54 RGB sentinel samples required.");
        foreach (int color in rgb) if (color < 0 || color > 0xffffff) throw new ArgumentException("RGB sample out of range.");
        int recognized = -1;
        for (int phase=0; phase<PhaseCount; ++phase) {
            bool match = true;
            for (int i=0; i<rgb.Length && match; ++i) for (int shift=0; shift<=16; shift+=8)
                if (Math.Abs(((rgb[i]>>shift)&255)-((palette[(i/9+phase)%6]>>shift)&255)) > ColorTolerance) match=false;
            if (match) { if (recognized != -1) return -1; recognized=phase; }
        }
        return recognized;
    }
    static bool Owned(IntPtr window, uint pid) { uint owner; return window!=IntPtr.Zero && GetWindowThreadProcessId(window,out owner)!=0 && owner==pid; }
    static bool Intersects(Rect a, Rect b) { return a.left<b.right && b.left<a.right && a.top<b.bottom && b.top<a.bottom; }
    static bool Same(Rect a, Rect b) { return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom; }
    static bool Uncloaked(IntPtr window) { uint value; return DwmGetWindowAttribute(window,14,out value,4)==0 && value==0; }
    static Point[] Points(Rect rect) {
        var result=new List<Point>();
        foreach (double y in new[]{.28,.72}) foreach (double x in new[]{.18,.50,.82})
            foreach (int dy in new[]{-2,0,2}) foreach (int dx in new[]{-2,0,2})
                result.Add(new Point(rect.left+(int)((rect.right-rect.left)*x)+dx,rect.top+(int)((rect.bottom-rect.top)*y)+dy));
        return result.ToArray();
    }
    static string CursorOcclusion(Rect canvas) {
        var cursor=new CursorInfo { size=Marshal.SizeOf(typeof(CursorInfo)) };
        if (!GetCursorInfo(ref cursor)) return "cursor-state-unavailable";
        if ((cursor.flags&1)==0) return null;
        IconInfo icon;
        if (!GetIconInfo(cursor.cursor,out icon)) return "cursor-bounds-unavailable";
        try {
            Bitmap bitmap;
            if (GetObject(icon.color!=IntPtr.Zero?icon.color:icon.mask,Marshal.SizeOf(typeof(Bitmap)),out bitmap)==0) return "cursor-bounds-unavailable";
            var rect=new Rect { left=cursor.position.x-(int)icon.xHotspot, top=cursor.position.y-(int)icon.yHotspot };
            rect.right=rect.left+bitmap.width; rect.bottom=rect.top+bitmap.height/(icon.color==IntPtr.Zero?2:1);
            return Intersects(canvas,rect)?"cursor-occludes-canvas":null;
        } finally { if(icon.mask!=IntPtr.Zero) DeleteObject(icon.mask); if(icon.color!=IntPtr.Zero) DeleteObject(icon.color); }
    }
    static string Guard(IntPtr root, IntPtr canvas, uint pid, out Rect rect) {
        rect=new Rect();
        if (!Owned(root,pid) || !Owned(canvas,pid) || GetAncestor(canvas,2)!=root || root==canvas) return "unowned-or-invalid-canvas";
        if (!IsWindowVisible(root) || !IsWindowVisible(canvas) || IsIconic(root)) return "hidden-or-minimized";
        if (!Uncloaked(root)) return "cloaked-or-unverified-root";
        Rect client; var point=new Point(0,0);
        if (!GetClientRect(canvas,out client) || !ClientToScreen(canvas,ref point)) return "canvas-geometry-unavailable";
        rect=new Rect { left=point.x,top=point.y,right=point.x+client.right-client.left,bottom=point.y+client.bottom-client.top };
        if (rect.right-rect.left<320 || rect.bottom-rect.top<180) return "canvas-too-small";
        var monitor=new MonitorInfo { size=Marshal.SizeOf(typeof(MonitorInfo)) };
        if (!GetMonitorInfo(MonitorFromWindow(canvas,0),ref monitor) || rect.left<monitor.monitor.left || rect.top<monitor.monitor.top || rect.right>monitor.monitor.right || rect.bottom>monitor.monitor.bottom) return "canvas-not-entirely-on-one-monitor";
        for (IntPtr above=GetWindow(root,3); above!=IntPtr.Zero; above=GetWindow(above,3)) {
            Rect bounds;
            if (!IsWindowVisible(above)) continue;
            if (!GetWindowRect(above,out bounds)) return "occluder-geometry-unavailable";
            if (Intersects(rect,bounds)) {
                uint cloaked;
                if (DwmGetWindowAttribute(above,14,out cloaked,4)!=0 || cloaked==0) return "higher-window-intersects-canvas";
            }
        }
        foreach (Point sample in Points(rect)) {
            IntPtr hit=WindowFromPoint(sample);
            if (!Owned(hit,pid) || (hit!=canvas && !IsChild(canvas,hit))) return "sentinel-occluded-or-unowned";
        }
        return CursorOcclusion(rect);
    }
    public static Sample Capture(IntPtr root, IntPtr canvas, uint pid) {
        var sample=new Sample { qpcStart=Stopwatch.GetTimestamp(),root=root.ToInt64(),canvas=canvas.ToInt64(),status="rejected" };
        IntPtr previous=SetThreadDpiAwarenessContext(new IntPtr(-4));
        if (previous==IntPtr.Zero) { sample.reason="physical-coordinate-context-unavailable"; sample.qpcEnd=Stopwatch.GetTimestamp(); return sample; }
        try {
            Rect before;
            sample.reason=Guard(root,canvas,pid,out before);
            if (sample.reason!=null) return sample;
            uint beforeDpi=GetDpiForWindow(canvas);
            if (beforeDpi==0) { sample.reason="canvas-dpi-unavailable";return sample; }
            var values=new int[54]; var points=Points(before);
            IntPtr dc=GetDC(IntPtr.Zero);
            if (dc==IntPtr.Zero) { sample.reason="desktop-dc-unavailable"; return sample; }
            try {
                sample.qpcReadStart=Stopwatch.GetTimestamp();
                for (int i=0;i<points.Length;++i) {
                    IntPtr hit=WindowFromPoint(points[i]);
                    if (!Owned(hit,pid) || (hit!=canvas && !IsChild(canvas,hit))) { sample.reason="sentinel-occluded-during-readback";return sample; }
                    uint value=GetPixel(dc,points[i].x,points[i].y);
                    if (value==0xffffffff) { sample.reason="desktop-pixel-unavailable"; return sample; }
                    values[i]=(int)(((value&255)<<16)|(value&0xff00)|((value>>16)&255));
                }
                sample.qpcReadEnd=Stopwatch.GetTimestamp();
            } finally { ReleaseDC(IntPtr.Zero,dc); }
            Rect after;
            sample.reason=Guard(root,canvas,pid,out after);
            if (sample.reason!=null || !Same(before,after)) { if(sample.reason==null)sample.reason="canvas-moved-during-readback"; return sample; }
            if (GetDpiForWindow(canvas)!=beforeDpi) { sample.reason="canvas-dpi-changed";return sample; }
            sample.screenCanvas=after; sample.dpi=beforeDpi; sample.rgb=values;
            sample.phase=RecognizeRgb(values); sample.status="sampled"; sample.safe=true;
            return sample;
        } finally { SetThreadDpiAwarenessContext(previous); sample.qpcEnd=Stopwatch.GetTimestamp(); }
    }
    static Sample Discover(uint pid, string player) {
        if (pid==0 || (player!="Aperture" && player!="VLC")) throw new ArgumentException("Explicit owned player PID and supported player required.");
        long start=Stopwatch.GetTimestamp(); var matches=new List<IntPtr[]>();
        EnumWindows((root,context)=> {
            if (!Owned(root,pid) || !IsWindowVisible(root) || GetWindow(root,4)!=IntPtr.Zero) return true;
            EnumChildWindows(root,(child,unused)=> {
                if (!Owned(child,pid) || !IsWindowVisible(child)) return true;
                var name=new StringBuilder(256); GetClassName(child,name,name.Capacity);
                if ((player=="Aperture" && name.ToString()=="mpv") || (player=="VLC" && name.ToString().StartsWith("VLC video main ",StringComparison.Ordinal))) matches.Add(new[]{root,child});
                return true;
            },IntPtr.Zero);
            return true;
        },IntPtr.Zero);
        if (matches.Count!=1) return new Sample { status=matches.Count==0?"pending":"rejected",reason=matches.Count==0?"video-canvas-not-present":"ambiguous-video-canvases",qpcStart=start,qpcEnd=Stopwatch.GetTimestamp() };
        return new Sample { status="discovered",root=matches[0][0].ToInt64(),canvas=matches[0][1].ToInt64(),qpcStart=start,qpcEnd=Stopwatch.GetTimestamp() };
    }
    public static Sample ValidateVisibleCanvas(IntPtr root, IntPtr canvas, uint pid) {
        var sample=new Sample { qpcStart=Stopwatch.GetTimestamp(),root=root.ToInt64(),canvas=canvas.ToInt64(),status="rejected" };
        IntPtr previous=SetThreadDpiAwarenessContext(new IntPtr(-4));
        if (previous==IntPtr.Zero) { sample.reason="physical-coordinate-context-unavailable";sample.qpcEnd=Stopwatch.GetTimestamp();return sample; }
        try {
            Rect rect;
            sample.reason=Guard(root,canvas,pid,out rect);
            if (sample.reason!=null) return sample;
            sample.dpi=GetDpiForWindow(canvas);
            if (sample.dpi==0) { sample.reason="canvas-dpi-unavailable";return sample; }
            sample.screenCanvas=rect;sample.safe=true;sample.status="validated";
            return sample;
        } finally { SetThreadDpiAwarenessContext(previous);sample.qpcEnd=Stopwatch.GetTimestamp(); }
    }
    public static Sample ValidateVisibleCanvas(uint pid, string player) {
        Sample found=Discover(pid,player);
        return found.status=="discovered"?ValidateVisibleCanvas(new IntPtr(found.root),new IntPtr(found.canvas),pid):found;
    }
    public static Sample Observe(uint pid, string player) {
        Sample found=Discover(pid,player);
        return found.status=="discovered"?Capture(new IntPtr(found.root),new IntPtr(found.canvas),pid):found;
    }
    public static Result WaitForPhase(uint pid, string player, long originQpc, int expectedPhase, int timeoutMilliseconds=15000, int pollMilliseconds=5) {
        if (expectedPhase<0 || expectedPhase>=PhaseCount || timeoutMilliseconds<1 || timeoutMilliseconds>60000 || pollMilliseconds<1 || pollMilliseconds>100) throw new ArgumentOutOfRangeException("phase/timeout/poll");
        long started=Stopwatch.GetTimestamp();
        if (originQpc<=0 || originQpc>started) throw new ArgumentException("Origin must be a preceding QPC timestamp.");
        var result=new Result { pid=pid,expectedPhase=expectedPhase,originQpc=originQpc,observerStartedQpc=started,pollMilliseconds=pollMilliseconds,status="timeout",reason="known-phase-not-observed" };
        bool previousMatched=false;
        try {
            using (var process=Process.GetProcessById(checked((int)pid))) {
                result.processStartedUtc=process.StartTime.ToUniversalTime().ToString("o");
                while ((Stopwatch.GetTimestamp()-started)*1000.0/Stopwatch.Frequency<timeoutMilliseconds) {
                    if (process.HasExited) { result.status="rejected";result.reason="owned-process-exited";return result; }
                    Sample sample=Observe(pid,player);
                    if (sample.status=="rejected") { result.status="rejected";result.reason=sample.reason;result.lastNegative=sample;return result; }
                    if (sample.status=="sampled") ++result.capturedSamples; else ++result.pendingPolls;
                    if (sample.status=="sampled" && sample.phase==expectedPhase) {
                        if (result.firstPositive==null) result.firstPositive=sample;
                        if (previousMatched) {
                            result.status="recognized";result.reason=null;result.confirmation=sample;
                            result.leftCensored=result.lastNegative==null;
                            result.lowerBoundMs=((result.lastNegative==null?originQpc:result.lastNegative.qpcStart)-originQpc)*1000.0/Stopwatch.Frequency;
                            result.upperBoundMs=(result.firstPositive.qpcReadEnd-originQpc)*1000.0/Stopwatch.Frequency;
                            result.confirmationMs=(sample.qpcReadEnd-originQpc)*1000.0/Stopwatch.Frequency;
                            return result;
                        }
                        previousMatched=true;
                    } else { if(sample.status=="sampled")++result.nonmatchingSamples;if(result.firstPositive==null)result.lastNegative=sample;previousMatched=false; }
                    Thread.Sleep(pollMilliseconds);
                }
            }
        } catch (ArgumentException) { result.status="rejected";result.reason="process-or-player-unavailable"; }
          catch (InvalidOperationException) { result.status="rejected";result.reason="owned-process-unavailable"; }
          catch (System.ComponentModel.Win32Exception) { result.status="rejected";result.reason="owned-process-query-failed"; }
          catch (OverflowException) { result.status="rejected";result.reason="invalid-process-id"; }
        return result;
    }
}
