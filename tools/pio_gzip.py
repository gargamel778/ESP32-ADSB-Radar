# PlatformIO pre-build script: gzip firmware/data/page.html -> page.html.gz (the web page is embedded and served compressed).
# Runs at every build; the output is deterministic (no timestamp) so unchanged input gives an unchanged file.
import gzip, os
Import("env")  # noqa: F821  (provided by PlatformIO/SCons)
proj = env.subst("$PROJECT_DIR")  # noqa: F821
src = os.path.join(proj, "data", "page.html")
dst = src + ".gz"
data = open(src, "rb").read()
packed = gzip.compress(data, compresslevel=9, mtime=0)
if not os.path.exists(dst) or open(dst, "rb").read() != packed:
    open(dst, "wb").write(packed)
    print("page.html: %d -> %d bytes (gzip)" % (len(data), len(packed)))
