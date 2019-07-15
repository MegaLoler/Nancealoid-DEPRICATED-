tract: tract.c
	gcc tract.c -ljack -lm -Wall -o tract

clean: tract
	rm tract

run: tract
	./tract
