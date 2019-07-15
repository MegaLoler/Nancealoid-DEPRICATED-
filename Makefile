nancealoid:
	gcc main.c -Wall -o nancealoid

clean: nancealoid
	rm nancealoid

run: nancealoid
	./nancealoid
