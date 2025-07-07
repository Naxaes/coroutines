main: build main.c
	clang main.c -o build/main -Wall -Werror -Wno-unused-variable -DLOG

test: build main.c
	clang main.c -o build/test -Wall -Werror -Wno-unused-variable

build:
	mkdir -p build