param(
    [string]$OutDir = '',
    [string]$Ffmpeg = 'ffmpeg',
    [switch]$ExtendedOnly
)
$ErrorActionPreference = "Stop"
if (!$OutDir) { $OutDir = Join-Path $PSScriptRoot '..\..\scratch\libmpv-tests\media' }
$OutDir = [IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$ffmpeg = $Ffmpeg
$common = @("-hide_banner", "-loglevel", "error", "-y")

function Run([string[]]$Args2) {
    & $ffmpeg @common @Args2
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed: $($Args2 -join ' ')" }
}

# 720p testsrc-like moving content (mandelbrot is compressible-free, good decode load)
if (-not $ExtendedOnly) {
Run @("-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=20",
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=20",
      "-c:v", "libx264", "-preset", "fast", "-pix_fmt", "yuv420p",
      "-c:a", "aac", "-b:a", "128k", "-shortest",
      "-movflags", "+faststart", "$OutDir\h264_aac_720p.mp4")

# 5.1 audio variant (channel mapping / night mode / downmix tests)
Run @("-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=20",
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=20",
      "-filter_complex", "[1:a]aformat=channel_layouts=5.1[a1]",
      "-map", "0:v", "-map", "[a1]",
      "-c:v", "libx264", "-preset", "fast", "-pix_fmt", "yuv420p",
      "-c:a", "aac", "-b:a", "384k", "-shortest",
      "$OutDir\h264_aac_51_720p.mp4")

# HEVC
Run @("-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=20",
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=20",
      "-c:v", "libx265", "-preset", "fast", "-pix_fmt", "yuv420p", "-tag:v", "hvc1",
      "-c:a", "aac", "-b:a", "128k", "-shortest",
      "$OutDir\hevc_aac_720p.mp4")

# AV1 + Opus in MKV
Run @("-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=20",
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=20",
      "-c:v", "libsvtav1", "-preset", "8", "-pix_fmt", "yuv420p",
      "-c:a", "libopus", "-b:a", "96k", "-shortest",
      "$OutDir\av1_opus.mkv")

# VP9 + Opus in WebM
Run @("-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=20",
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=20",
      "-c:v", "libvpx-vp9", "-b:v", "0", "-crf", "40", "-pix_fmt", "yuv420p",
      "-c:a", "libopus", "-b:a", "96k", "-shortest",
      "$OutDir\vp9_opus.webm")

# MPEG-4 ASP + MP3 in AVI (legacy)
Run @("-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=20",
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=20",
      "-c:v", "mpeg4", "-q:v", "5", "-pix_fmt", "yuv420p",
      "-c:a", "libmp3lame", "-b:a", "128k", "-shortest",
      "$OutDir\mpeg4_mp3.avi")

# WMV3/WMA in ASF
Run @("-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=20",
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=20",
      "-c:v", "wmv2", "-q:v", "5", "-pix_fmt", "yuv420p",
      "-c:a", "wmav2", "-shortest",
      "$OutDir\wmv_wma.wmv")

# Audio-only formats
Run @("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=20",
      "-c:a", "libmp3lame", "-b:a", "192k", "$OutDir\audio_mp3.mp3")
Run @("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=20",
      "-c:a", "flac", "$OutDir\audio_flac.flac")
Run @("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=20",
      "-filter_complex", "[0:a]aformat=channel_layouts=5.1[a1]",
      "-map", "[a1]", "-c:a", "pcm_s16le", "$OutDir\audio_51.wav")

# Subtitle files
@"
1
00:00:00,000 --> 00:00:10,000
Hello from SRT subtitle

2
00:00:10,000 --> 00:00:20,000
Second subtitle line
"@ | Set-Content -Path "$OutDir\test_subs.srt" -Encoding UTF8

# Embedded mov_text subtitles in mp4
Run @("-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=20",
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=20",
      "-i", "$OutDir\test_subs.srt",
      "-map", "0:v", "-map", "1:a", "-map", "2:s",
      "-c:v", "libx264", "-preset", "fast", "-pix_fmt", "yuv420p",
      "-c:a", "aac", "-b:a", "128k",
      "-c:s", "mov_text", "-shortest",
      "$OutDir\h264_subs_movtext.mp4")

# 60s 1080p decode benchmark file (higher decode load)
Run @("-f", "lavfi", "-i", "mandelbrot=size=1920x1080:rate=30",
      "-t", "60",
      "-c:v", "libx264", "-preset", "fast", "-pix_fmt", "yuv420p", "-an",
      "$OutDir\h264_1080p_60s.mp4")

}
Write-Host "Media generated in $OutDir"
Get-ChildItem $OutDir | Format-Table Name, Length -AutoSize


Run @("-i", "$OutDir\h264_aac_720p.mp4", "-c", "copy", "-movflags", "+faststart", "$OutDir\h264_aac_faststart.mp4")
Run @("-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=10",
      "-c:v", "libx265", "-preset", "fast", "-pix_fmt", "yuv420p10le", "-an",
      "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
      "-x265-params", "hdr10=1:repeat-headers=1:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400",
      "$OutDir\hevc_hdr10.mp4")
@"
[Script Info]
ScriptType: v4.00+
PlayResX: 1280
PlayResY: 720
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,42,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.00,0:00:20.00,Default,,0,0,0,,ASS integration subtitle
"@ | Set-Content -LiteralPath "$OutDir\test_subs.ass" -Encoding UTF8
