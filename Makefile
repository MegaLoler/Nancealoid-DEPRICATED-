nancealoid: main.c
	gcc main.c -ljack -lm -Wall -o nancealoid

clean: nancealoid
	rm nancealoid

run: nancealoid
	./nancealoid
