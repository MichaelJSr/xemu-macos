#!/bin/bash
set -ex
# uv tool install fonttools
# No GNU-only grep/sed/paste forms, so it also runs on macOS. Works from any
# directory.
cd "$(dirname "$0")/../data"
FONT_INPUT=font_awesome_6_1_1_solid.otf
FONT_OUTPUT=font_awesome_6_1_1_solid.min.otf
GLYPHS=$(grep -rhoE "ICON_FA_[A-Z0-9_]+" ../ui/xui | sort | uniq | sed -E 's/^ICON_FA_//; s/_/-/g' | tr '[:upper:]' '[:lower:]' | paste -sd "," -)
pyftsubset $FONT_INPUT --output-file=$FONT_OUTPUT --glyphs=$GLYPHS
