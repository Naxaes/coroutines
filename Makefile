

main: build main.c
	cc -arch x86_64 main.c -o build/main

build:
	mkdir -p build