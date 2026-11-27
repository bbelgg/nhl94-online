#!/bin/bash
# Double-click in Finder: start NHL94Recomp (the launcher) from this folder.
# The control mode - Classic or Position lock - is chosen in the launcher:
# Mods > Gameplay > Control mode. Online the HOST's mode is used by both.
cd "$(dirname "$0")" || exit 1
exec ./NHL94Recomp "$@"
