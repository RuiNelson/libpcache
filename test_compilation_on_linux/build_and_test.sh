#!/bin/sh
set -e
cd "$(dirname "$0")/.."
DOCKER_BUILDKIT=1 docker build --progress=plain -t libpcache-test -f test_compilation_on_linux/Dockerfile .
docker rmi libpcache-test
