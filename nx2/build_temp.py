"""Temporary directories for the Xbox builder and its child scripts."""
import contextlib
import os
import secrets
import shutil
import tempfile


@contextlib.contextmanager
def temporary_directory(prefix="tmp", dir=None):
    xbox = os.name == "nt" and os.environ.get("LONGSHOT_XBOX", "").strip().lower() not in ("", "0", "false", "off", "no")
    if not xbox:
        with tempfile.TemporaryDirectory(prefix=prefix, dir=dir) as path:
            yield path
        return

    parent = os.path.abspath(dir or tempfile.gettempdir())
    for _ in range(100):
        path = os.path.join(parent, prefix + secrets.token_hex(16))
        try:
            # Windows Python 3.13+ mode 0700 installs a protected ACL that may exclude the app container.
            os.mkdir(path, 0o777)
            break
        except FileExistsError:
            continue
    else:
        raise FileExistsError("Could not create a unique build temporary directory in " + parent)
    try:
        yield path
    finally:
        shutil.rmtree(path)
