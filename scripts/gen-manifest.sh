#!/bin/sh
# Emits the goose-build manifest to stdout, derived from the shaders/ layout:
#   shaders/common.slang      shared uniform types (imported, no entry points)
#   shaders/scenes/<n>.slang  one self-contained scene per file
#                             (vertMain + pixelMain, like GIS blit.slang)
# Run from the repo root. The manifest is a build product (gitignored);
# scene files are the single source of truth — dropping a file in scenes/
# and running make adds it.
set -eu
{
    printf '{\n'
    printf '  "outputDir": "build/generated",\n'
    printf '  "parameterOutputDir": "build/generated",\n'
    printf '  "package": "generated",\n'
    printf '  "gooseImport": "unused",\n'
    printf '  "targets": ["metal", "spirv"],\n'
    printf '  "shaders": [\n'
    first=1
    for f in shaders/scenes/*.slang; do
        name=$(basename "$f" .slang)
        if [ "$first" -eq 1 ]; then first=0; printf '    '; else printf ',\n    '; fi
        printf '{"name": "%s", "source": "%s"}' "$name" "$f"
    done
    printf '\n  ]\n}\n'
}
