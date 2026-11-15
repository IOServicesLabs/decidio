"""Write every installable idea build under installer/builds/ (the files installer/index.html flashes from the browser).

    python tools/make_builds.py [--out installer/builds]

Run it after changing recipes/*.yaml, a base model or the firmware. It first re-checks the recipes against the base
models (needs pyyaml), then packages each idea: seconds, no training. The output is generated; publish the whole
installer/ folder over HTTPS (browsers only allow USB flashing from https:// or localhost).
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from decidio import builds, recipes  # noqa: E402

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=str(builds.OUT))
    a = ap.parse_args()
    recipes.build()
    builds.make(Path(a.out))
