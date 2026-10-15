"""Rebuild resources/initial_pipeline_cache.db from a played-in pipeline cache.

Aurora skips a draw while its pipeline is still compiling, so a machine with an
empty cache drops geometry (black patches) the first time each material shows
up. A fresh install merges the seed into its own cache at startup and compiles
every pipeline in it in the background, during boot and the front end.

The cache rows are render-state descriptions only -- blend, depth, cull, TEV
combiner and vertex-layout enums. No textures, models or code from the disc.

    python tools/refresh-pipeline-seed.py [path/to/pipeline_cache.db]

Default source is %APPDATA%/openstrikers/pipeline_cache.db (Windows) or
~/.config/openstrikers/pipeline_cache.db. Play a full match or two first,
including a goal replay and the end-of-game screens, to cover more shaders.
Re-run after an Aurora update that bumps GXPipelineConfigVersion: rows with a
stale version are skipped when seeding.
"""
import os
import sqlite3
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEST = REPO / "resources" / "initial_pipeline_cache.db"


def default_source() -> Path:
    if os.name == "nt":
        appdata = Path(os.environ["APPDATA"])
        return appdata / "openstrikers" / "pipeline_cache.db"
    return Path.home() / ".config" / "openstrikers" / "pipeline_cache.db"


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else default_source()
    if not src.is_file():
        print(f"no pipeline cache at {src}", file=sys.stderr)
        return 1

    DEST.parent.mkdir(exist_ok=True)
    tmp = DEST.with_suffix(".tmp")
    tmp.unlink(missing_ok=True)
    # The backup API takes a consistent snapshot even if the game is running.
    s = sqlite3.connect(f"file:{src}?mode=ro", uri=True)
    d = sqlite3.connect(tmp)
    s.backup(d)
    s.close()
    d.execute("PRAGMA journal_mode=DELETE")
    d.execute("VACUUM")
    rows = d.execute(
        "SELECT type, config_version, COUNT(*)"
        " FROM pipeline_cache GROUP BY 1, 2"
    ).fetchall()
    d.close()
    os.replace(tmp, DEST)

    for kind, version, count in rows:
        print(f"type {kind} config_version {version}: {count} pipelines")
    print(f"wrote {DEST} ({DEST.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
