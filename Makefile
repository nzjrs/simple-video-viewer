svv: svv.c
	pkg-config libv4lconvert --atleast-version=0.5.8
	$(CC) -Wall $? -o $@ -DWITH_GTK=0 `pkg-config libv4lconvert libv4l2 gtk+-2.0 --cflags --libs`

clean:
	rm -f svv
