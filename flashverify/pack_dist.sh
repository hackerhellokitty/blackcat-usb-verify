#!/usr/bin/env bash
# pack_dist.sh — pack FlashVerify into a distributable folder + zip

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD="$SCRIPT_DIR/builddir"
DIST="$SCRIPT_DIR/FlashVerify-dist"

echo "==> Cleaning old dist..."
rm -rf "$DIST"
mkdir -p "$DIST/python"
mkdir -p "$DIST/share/glib-2.0/schemas"

echo "==> Copying exe..."
cp "$BUILD/flashverify.exe" "$DIST/"

echo "==> Copying icon..."
cp "$SCRIPT_DIR/flash-disk.ico" "$DIST/"

echo "==> Copying DLLs..."
cp "$BUILD"/*.dll "$DIST/"

echo "==> Copying GSettings schemas..."
cp "$BUILD/share/glib-2.0/schemas/"* "$DIST/share/glib-2.0/schemas/" 2>/dev/null || true

echo "==> Copying python script..."
cp "$SCRIPT_DIR/python/generate_report.py" "$DIST/python/"

echo "==> Copying Thai font (if present)..."
for f in NotoSansThai-Regular.ttf NotoSansThai-Bold.ttf NotoSans-Regular.ttf NotoSans-Bold.ttf; do
    [ -f "$SCRIPT_DIR/python/$f" ] && cp "$SCRIPT_DIR/python/$f" "$DIST/python/" && echo "  font: $f"
done

echo "==> Setting up Python venv in dist (with system-site-packages)..."
MSYS2_PYTHON="C:/msys64/ucrt64/bin/python.exe"
if [ -f "$MSYS2_PYTHON" ]; then
    "$MSYS2_PYTHON" -m venv --system-site-packages "$DIST/python/venv"
    echo "  venv created"
else
    echo "  WARNING: MSYS2 python not found at $MSYS2_PYTHON"
fi

echo ""
echo "==> Creating zip..."
cd "$SCRIPT_DIR"
ZIPNAME="FlashVerify-dist.zip"
rm -f "$ZIPNAME"
powershell.exe -NoProfile -Command "Compress-Archive -Path 'FlashVerify-dist' -DestinationPath '$ZIPNAME' -Force"

echo ""
echo "Done!"
echo "  Folder : $DIST"
echo "  Zip    : $SCRIPT_DIR/$ZIPNAME"
ls -lh "$SCRIPT_DIR/$ZIPNAME" 2>/dev/null || true
