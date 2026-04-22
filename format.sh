#!/bin/bash
# Format all source files with clang-format before committing

cd "$(dirname "$0")"

echo "Formatting source files..."
clang-format -i src/*.c include/*.h tests/*.c

echo "Done"
