svv: svv.c
	pkg-config libv4lconvert --atleast-version=0.5.8
	$(CC) -Wall $? -o $@ `pkg-config libv4lconvert gtk+-2.0 --cflags --libs`

clean:
	rm -f svv
