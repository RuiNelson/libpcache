#!/bin/bash
set -e

cmake -B build -Wno-deprecated
cmake --build build
ctest --test-dir build
