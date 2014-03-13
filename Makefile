magnetophon: magnetophon.cpp
	gcc -Wall -o magnetophon magnetophon.cpp -framework AudioToolbox -framework CoreAudio -framework CoreFoundation

clean:
	-rm -f magnetophon.o

