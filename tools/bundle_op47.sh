#!/bin/bash
# Bundle op47_extract with all dependencies for portable deployment
set -e

OUTDIR="${1:-op47_bundle}"
BINARY="tools/op47_extract"

if [ ! -f "$BINARY" ]; then
    echo "Error: $BINARY not found. Run 'make tools/op47_extract' first."
    exit 1
fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/lib"

# Copy binary as .bin
cp "$BINARY" "$OUTDIR/op47_extract.bin"

# Copy all shared library dependencies
ldd "$BINARY" | awk '/=>/ && !/linux-vdso/ {print $3}' | while read lib; do
    if [ -f "$lib" ]; then
        cp -L "$lib" "$OUTDIR/lib/"
    fi
done

# Also grab the ld-linux loader
LOADER=$(ldd "$BINARY" | grep 'ld-linux' | awk '{print $1}')
if [ -f "$LOADER" ]; then
    cp -L "$LOADER" "$OUTDIR/lib/"
fi

# Create wrapper script
cat > "$OUTDIR/op47_extract" << 'WRAPPER'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/lib/ld-linux-x86-64.so.2" --library-path "$DIR/lib" "$DIR/op47_extract.bin" "$@"
WRAPPER
chmod +x "$OUTDIR/op47_extract"

echo "Bundle created in $OUTDIR/"
echo "Copy entire directory to target machine and run ./op47_extract"
du -sh "$OUTDIR"
