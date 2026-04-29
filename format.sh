#!/bin/bash
# Format all source files with clang-format before committing

cd "$(dirname "$0")"

echo "Formatting source files..."
clang-format -i src/*.c src/*.h include/*.h tests/*.c tests/*.h
clang-format -i repl/src/*.cxx repl/src/*.hxx

echo "Done"
