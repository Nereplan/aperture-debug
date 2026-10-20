"""Generate a new controlled H.264/AAC sentinel fixture without opening a window."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

PALETTE = [0xE62828, 0x28D73C, 0x3246E6, 0x1ED2DC, 0xDC28D2, 0xE6D21E]
DEFINITION = {"schemaVersion": 1, "width": 1280, "height": 720, "fps": 30,
              "phaseSeconds": 12, "phaseCount": 4, "paletteRgb": PALETTE,
              "centers": [[x, y] for y in (.28, .72) for x in (.18, .50, .82)],
              "phasePattern": "Patch i uses palette[(i + phase) % 6]; phases are half-open 12s intervals.",
              "scope": "Synthetic readiness fixture; sparse sentinel visibility is not general image-quality, presentation-cadence or physical-audio validation."}


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(arguments, **kwargs):
    return subprocess.run(arguments, check=True, stdin=subprocess.DEVNULL,
                          creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--describe", action="store_true", help="Print the recognition definition without encoding")
    parser.add_argument("--output-dir", type=Path, help="A new external scratch directory; never overwritten")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    if args.describe:
        print(json.dumps(DEFINITION))
        return
    parser.error("--output-dir is required") if args.output_dir is None else None
    ffmpeg, ffprobe = shutil.which(args.ffmpeg), shutil.which(args.ffprobe)
    if not ffmpeg or not ffprobe:
        parser.error("Supply existing ffmpeg and ffprobe executables; nothing is installed/downloaded")
    args.output_dir.mkdir(parents=True, exist_ok=False)
    output = args.output_dir / "video-readiness-h264-aac.mp4"
    filters = []
    for phase in range(4):
        for index, (x, y) in enumerate(DEFINITION["centers"]):
            color = PALETTE[(index + phase) % 6]
            filters.append(f"drawbox=x={round((x-.10)*1280)}:y={round((y-.13)*720)}:w=256:h=188:"
                           f"color=0x{color:06x}:t=fill:enable='gte(t,{phase*12})*lt(t,{(phase+1)*12})'")
    command = [ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-n",
               "-f", "lavfi", "-i", "color=c=0x141414:s=1280x720:r=30:d=48",
               "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=48",
               "-vf", ",".join(filters), "-c:v", "libx264", "-preset", "veryfast", "-crf", "18",
               "-threads", "1", "-g", "30", "-keyint_min", "30", "-sc_threshold", "0",
               "-pix_fmt", "yuv420p", "-c:a", "aac", "-ac", "2", "-ar", "48000",
               "-b:a", "128k", "-movflags", "+faststart", "-shortest", str(output)]
    with (args.output_dir / "ffmpeg.log").open("wb") as log:
        run(command, stdout=log, stderr=subprocess.STDOUT)
    probe = json.loads(run([ffprobe, "-v", "error", "-show_entries",
                           "stream=codec_name,codec_type,width,height,pix_fmt,avg_frame_rate,channels,sample_rate",
                           "-of", "json", str(output)], capture_output=True, text=True).stdout)
    video = next(s for s in probe["streams"] if s["codec_type"] == "video")
    audio = next(s for s in probe["streams"] if s["codec_type"] == "audio")
    if not (video["codec_name"] == "h264" and video["width"] == 1280 and video["height"] == 720
            and video["pix_fmt"] == "yuv420p" and video["avg_frame_rate"] == "30/1"
            and audio["codec_name"] == "aac" and audio["channels"] == 2 and audio["sample_rate"] == "48000"):
        raise ValueError("Generated fixture stream metadata differs from the protocol")
    manifest = dict(DEFINITION, mediaPath=str(output.resolve()), mediaBytes=output.stat().st_size,
                    mediaSha256=digest(output), streams=probe["streams"], command=command,
                    ffmpegSha256=digest(ffmpeg), generatorSha256=digest(__file__),
                    observerSha256=digest(Path(__file__).with_name("PlayerVideoReadiness.cs")),
                    ffmpegVersion=run([ffmpeg, "-version"], capture_output=True, text=True).stdout.splitlines()[0])
    with (args.output_dir / "manifest.json").open("x", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")
    print(args.output_dir / "manifest.json")


if __name__ == "__main__":
    main()
