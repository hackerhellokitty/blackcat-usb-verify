#!/bin/bash
# รันใน MSYS2 UCRT64 terminal
# copy DLL ทั้งหมดที่ flashverify.exe ต้องการมาไว้ใน builddir

set -e
BUILDDIR="$(dirname "$0")/builddir"
cd "$BUILDDIR"

echo "==> Copying required DLLs to: $BUILDDIR"
ldd flashverify.exe \
  | grep -i 'ucrt64\|mingw' \
  | awk '{print $3}' \
  | grep -v '^$' \
  | while read dll; do
      echo "  $dll"
      cp "$dll" .
    done

# GTK4 needs schema files for GSettings
SCHEMA_SRC="/ucrt64/share/glib-2.0/schemas"
if [ -d "$SCHEMA_SRC" ]; then
  mkdir -p share/glib-2.0/schemas
  cp "$SCHEMA_SRC"/gschemas.compiled share/glib-2.0/schemas/ 2>/dev/null || true
fi

# GDK pixbuf loaders (for icons/images)
GDK_SRC="/ucrt64/lib/gdk-pixbuf-2.0"
if [ -d "$GDK_SRC" ]; then
  mkdir -p lib
  cp -r "$GDK_SRC" lib/ 2>/dev/null || true
fi

echo ""
echo "==> Done. You can now run flashverify.exe from Windows Explorer."
