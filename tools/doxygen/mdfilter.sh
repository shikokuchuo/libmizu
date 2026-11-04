#!/bin/sh
# Doxygen input filter for the markdown pages: GitHub's <picture> logo
# switch (light/dark sources) is not a tag doxygen knows; hand it the
# plain <img> element inside instead. Everything else passes through.
sed -e 's|<picture>.*\(<img [^>]*>\).*</picture>|\1|' "$1"
