svv: svv.c
	$(CC) -Wall $? -o $@ `pkg-config libv4lconvert gtk+-2.0 --cflags --libs`

clean:
	rm -f svv
