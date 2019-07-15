tract: tract.c
	gcc tract.c -ljack -Wall -o tract

clean: tract
	rm tract

run: tract
	./tract
