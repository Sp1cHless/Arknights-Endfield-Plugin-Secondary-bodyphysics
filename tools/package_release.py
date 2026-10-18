"""Create and verify a ZIP before replacing the final release archive."""
import argparse
import os
from pathlib import Path
import sys
import tempfile
import time
import zipfile


def package(source, destination, attempts=4, retry_delay=0.5):
    source = Path(source).resolve()
    destination = Path(destination).resolve()
    if not source.is_dir():
        raise ValueError(f"Package directory missing: {source}")
    if destination == source or source in destination.parents:
        raise ValueError("The ZIP destination must be outside the package directory")
    files = sorted(path for path in source.rglob("*") if path.is_file())
    if not files:
        raise ValueError(f"Package directory is empty: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    for attempt in range(attempts):
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=destination.name + ".", suffix=".tmp", dir=destination.parent
        )
        os.close(descriptor)
        temporary = Path(temporary_name)
        try:
            snapshot = [(path.stat().st_size, path.stat().st_mtime_ns) for path in files]
            with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                for path in files:
                    # Python opens inputs for shared reading on Windows;
                    # another reader does not require exclusive file access.
                    archive.write(path, path.relative_to(source.parent).as_posix())
            if snapshot != [(path.stat().st_size, path.stat().st_mtime_ns) for path in files]:
                raise RuntimeError("Package source changed while creating the ZIP")
            with zipfile.ZipFile(temporary) as archive:
                bad_entry = archive.testzip()
                if bad_entry:
                    raise RuntimeError(f"ZIP integrity check failed: {bad_entry}")
            os.replace(temporary, destination)
            print(f"Package verified: {destination} ({len(files)} files)")
            return
        except PermissionError:
            if attempt + 1 >= attempts:
                raise
            print(f"Package file busy; retry {attempt + 1}/{attempts - 1}", file=sys.stderr)
            time.sleep(retry_delay * (attempt + 1))
        finally:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        package(args.source, args.destination)
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as error:
        print(f"[ERROR] Package not created: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
