
.PHONY: minigrep

minigrep: minigrep.c
	gcc -o minigrep minigrep.c -pthread

view: 
	less minigrep.c

run_threaded:
	./minigrep -P ~/ text

run_single:
	./minigrep -S ~/ text

clean:
	-rm *.o*
