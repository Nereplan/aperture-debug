"""Exercise real drag handlers in an isolated portable profile (Windows only)."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--exe", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
stage = args.output.resolve()
if stage == repo or stage == args.exe.resolve().parent:
    raise SystemExit("Use a separate, empty probe directory")
stage.mkdir(parents=True, exist_ok=False)
shutil.copy2(args.exe, stage / "Aperture.exe")
for folder in ("fonts", "locales"):
    if (repo / folder).is_dir():
        shutil.copytree(repo / folder, stage / folder)
(stage / "reorder.fixture").write_text("isolated interaction fixture")
(stage / "data").mkdir()
(stage / "media").mkdir()
items = [{"id": str(i), "title": f"Video {19-i:02d}", "filePath": f"media/{i}.mp4", "durationSec": 120,
          "videoWidth": 1920, "videoHeight": 1080} for i in range(20)]
for item in items:
    (stage / item["filePath"]).touch()
paths = [item["filePath"] for item in items]

def write(name, value):
    (stage / "data" / name).write_text(json.dumps(value), encoding="utf-8")

def entries(name):
    value = json.loads((stage / "data" / name).read_text(encoding="utf-8-sig"))
    if isinstance(value, list):
        return value
    for key in ("items", "mediaItems", "playlists"):
        if key in value:
            return value[key]
    raise AssertionError(f"Unknown format: {name}")

results = []
for scene in ("reorder_media_drop", "reorder_grid_drop", "reorder_list_drop", "reorder_media_cancel",
              "reorder_grid_capturelost", "reorder_grid_click", "reorder_grid_drag", "reorder_media_drag"):
    write("settings.json", {"autoUpdateCheckEnabled": False, "hoverPreviewEnabled": False,
        "restoreSession": False, "libraryGroupMode": "flat", "manualMediaOrder": False})
    write("library.json", items)
    write("playlists.json", [{"id": "test", "name": "Reorder fixture", "type": "manual", "itemPaths": paths}])
    write("history.json", [])
    write("watched_folders.json", [])
    result = subprocess.run([str(stage / "Aperture.exe"), "--visual-capture-probe",
        f"--visual-capture-scene={scene}", f"--visual-capture-output={stage / (scene + '.png')}"],
        timeout=40, capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
    assert result.returncode == 0, (scene, result.returncode, result.stderr)
    assert (stage / (scene + ".png")).exists(), scene
    actual_media = [item["id"] for item in entries("library.json")]
    actual_playlist = [Path(path.replace("\\", "/")).name for path in entries("playlists.json")[0]["itemPaths"]]
    expected_media = [str(i) for i in range(20)]
    expected_playlist = [f"{i}.mp4" for i in range(20)]
    if scene == "reorder_media_drop":
        expected_media = [str(i) for i in reversed(range(20))]
        expected_media.insert(2, expected_media.pop(0))
        settings = json.loads((stage / "data/settings.json").read_text(encoding="utf-8-sig"))
        assert settings["manualMediaOrder"] is True
    if scene in ("reorder_grid_drop", "reorder_list_drop"):
        expected_playlist.insert(3, expected_playlist.pop(0))
    assert actual_media == expected_media, (scene, "media", actual_media, expected_media)
    assert actual_playlist == expected_playlist, (scene, "playlist", actual_playlist, expected_playlist)
    results.append({"scene": scene, "passed": True})
    print(f"PASS {scene}", flush=True)
(stage / "results.json").write_text(json.dumps(results, indent=2))
