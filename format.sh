#/bin/sh
docker run --user $(id -u):$(id -g) -v `pwd`:`pwd` -w `pwd` -i --rm ghcr.io/jidicula/clang-format:15 -i Quake/*.c
docker run --user $(id -u):$(id -g) -v `pwd`:`pwd` -w `pwd` -i --rm ghcr.io/jidicula/clang-format:15 -i Quake/*.h
