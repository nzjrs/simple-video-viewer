/*
 *  Simple V4L2 video viewer.
 *
 *  This program can be used and distributed without restrictions.
 *
 * Generation (adjust to your libraries):

gcc -Wall svv.c -o svv $(pkg-config gtk+-2.0 --cflags --libs) -lv4lconvert

 */

/* comment these lines if you have not */
#define WITH_V4L2_LIB 1		/* v4l library */
#define WITH_GTK 1		/* gtk+ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>		/* getopt_long() */

#include <fcntl.h>		/* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <asm/types.h>		/* for videodev2.h */
#include <linux/videodev2.h>

#ifdef WITH_GTK
#include <gtk/gtk.h>
#endif

#ifdef WITH_V4L2_LIB
#include "libv4lconvert.h"
static struct v4lconvert_data *v4lconvert_data;
static struct v4l2_format src_fmt;	/* raw format */
static unsigned char *dst_buf;
#endif

#define IO_METHOD_READ 7	/* !! must be != V4L2_MEMORY_MMAP / USERPTR */

static struct v4l2_format fmt;		/* gtk pormat */

#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct buffer {
	void *start;
	size_t length;
};

static char *dev_name = "/dev/video0";
static int io = V4L2_MEMORY_MMAP;
static int fd = -1;
struct buffer *buffers;
static int n_buffers;
static int grab, raw;

static void errno_exit(const char *s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
#ifdef WITH_V4L2_LIB
	fprintf(stderr, "%s\n",
			v4lconvert_get_error_message(v4lconvert_data));
#endif
	exit(EXIT_FAILURE);
}

#ifdef WITH_GTK
static int read_frame(void);

/* graphic functions */
static GtkWidget *drawing_area;

static void delete_event(GtkWidget * widget, GdkEvent * event, gpointer data)
{
	gtk_main_quit();
}

static void frame_ready(gpointer data,
			gint source,
			GdkInputCondition condition)
{
	if (condition != GDK_INPUT_READ)
		errno_exit("poll");
	read_frame();
}


static int main_frontend(int argc, char *argv[])
{
	GtkWidget *window;

	gtk_init(&argc, &argv);
	gdk_rgb_init();

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), "My WebCam");

	gtk_signal_connect(GTK_OBJECT(window), "delete_event",
			   GTK_SIGNAL_FUNC(delete_event), NULL);
	gtk_signal_connect(GTK_OBJECT(window), "destroy",
			   GTK_SIGNAL_FUNC(delete_event), NULL);
	gtk_signal_connect(GTK_OBJECT(window), "key_press_event",
			   GTK_SIGNAL_FUNC(delete_event), NULL);

	gtk_container_set_border_width(GTK_CONTAINER(window), 2);

	drawing_area = gtk_drawing_area_new();
	gtk_drawing_area_size(GTK_DRAWING_AREA(drawing_area),
			      fmt.fmt.pix.width, fmt.fmt.pix.height);

	gtk_container_add(GTK_CONTAINER(window), drawing_area);

	gtk_widget_show_all(window);

	gdk_input_add(fd,
			GDK_INPUT_READ,
			frame_ready,
			NULL);
	gtk_main();

	return 0;
}
#endif

static int xioctl(int fd, int request, void *arg)
{
	int r;

	do {
		r = ioctl(fd, request, arg);
	} while (r < 0 && EINTR == errno);
	return r;
}

static void process_image(unsigned char *p, int len)
{
#ifdef WITH_V4L2_LIB
	if (!raw) {
		if (v4lconvert_convert(v4lconvert_data,
					&src_fmt,
					&fmt,
					p, len,
					dst_buf, fmt.fmt.pix.sizeimage) < 0) {
			if (errno != EAGAIN)
				errno_exit("v4l_convert");
			return;
		}
		p = dst_buf;
		len = fmt.fmt.pix.sizeimage;
	}
#endif
	if (grab) {
		FILE *f;

		f = fopen("image.dat", "w");
		fwrite(p, 1, len, f);
		fclose(f);
		if (raw)
			printf ("raw ");
		printf("image dumped to 'image.dat'\n");
		exit(EXIT_SUCCESS);
	}
#ifdef WITH_GTK
	gdk_draw_rgb_image(drawing_area->window,
			   drawing_area->style->white_gc,
			   0, 0,		/* xpos, ypos */
			   fmt.fmt.pix.width, fmt.fmt.pix.height,
//			   GDK_RGB_DITHER_MAX,
			   GDK_RGB_DITHER_NORMAL,
			   p,
			   fmt.fmt.pix.width * 3);
#else
	fputc('.', stdout);
#endif
}

static int read_frame(void)
{
	struct v4l2_buffer buf;
	int i;

	switch (io) {
	case IO_METHOD_READ:
		i = read(fd, buffers[0].start, buffers[0].length);
		if (i < 0) {
			switch (errno) {
			case EAGAIN:
				return 0;
			case EIO:
				/* Could ignore EIO, see spec. */
				/* fall through */
			default:
				errno_exit("read");
			}
		}
		process_image(buffers[0].start, i);
		break;

	case V4L2_MEMORY_MMAP:
		CLEAR(buf);
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			switch (errno) {
			case EAGAIN:
				return 0;

			case EIO:
				/* Could ignore EIO, see spec. */

				/* fall through */

			default:
				errno_exit("VIDIOC_DQBUF");
			}
		}
		assert(buf.index < n_buffers);

		process_image(buffers[buf.index].start, buf.bytesused);

		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0)
			errno_exit("VIDIOC_QBUF");
		break;
	case V4L2_MEMORY_USERPTR:
		CLEAR(buf);

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_USERPTR;

		if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			switch (errno) {
			case EAGAIN:
				return 0;
			case EIO:
				/* Could ignore EIO, see spec. */
				/* fall through */
			default:
				errno_exit("VIDIOC_DQBUF");
			}
		}

		for (i = 0; i < n_buffers; ++i)
			if (buf.m.userptr == (unsigned long) buffers[i].start
			    && buf.length == buffers[i].length)
				break;
		assert(i < n_buffers);

		process_image((unsigned char *) buf.m.userptr,
				buf.bytesused);

		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0)
			errno_exit("VIDIOC_QBUF");
		break;
	}
	return 1;
}

static int get_frame()
{
	fd_set fds;
	struct timeval tv;
	int r;

	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	/* Timeout. */
	tv.tv_sec = 2;
	tv.tv_usec = 0;

	r = select(fd + 1, &fds, NULL, NULL, &tv);
	if (r < 0) {
		if (EINTR == errno)
			return 0;

		errno_exit("select");
	}

	if (0 == r) {
		fprintf(stderr, "select timeout\n");
		exit(EXIT_FAILURE);
	}
	return read_frame();
}

#ifndef WITH_GTK
static void mainloop(void)
{
	int count;

//	count = 20;
	count = 10000;
	while (--count >= 0) {
		for (;;) {
			if (get_frame())
				break;
		}
	}
}
#endif

static void stop_capturing(void)
{
	enum v4l2_buf_type type;

	switch (io) {
	case IO_METHOD_READ:
		/* Nothing to do. */
		break;
	case V4L2_MEMORY_MMAP:
	case V4L2_MEMORY_USERPTR:
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
			errno_exit("VIDIOC_STREAMOFF");
		break;
	}
}

static void start_capturing(void)
{
	int i;
	enum v4l2_buf_type type;

	switch (io) {
	case IO_METHOD_READ:
		printf("read method\n");
		/* Nothing to do. */
		break;
	case V4L2_MEMORY_MMAP:
		printf("mmap method\n");
		for (i = 0; i < n_buffers; ++i) {
			struct v4l2_buffer buf;

			CLEAR(buf);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index = i;

			if (xioctl(fd, VIDIOC_QBUF, &buf) < 0)
				errno_exit("VIDIOC_QBUF");
		}

		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (xioctl(fd, VIDIOC_STREAMON, &type) < 0)
			errno_exit("VIDIOC_STREAMON");
		break;
	case V4L2_MEMORY_USERPTR:
		printf("userptr method\n");
		for (i = 0; i < n_buffers; ++i) {
			struct v4l2_buffer buf;

			CLEAR(buf);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_USERPTR;
			buf.index = i;
			buf.m.userptr = (unsigned long) buffers[i].start;
			buf.length = buffers[i].length;

			if (xioctl(fd, VIDIOC_QBUF, &buf) < 0)
				errno_exit("VIDIOC_QBUF");
		}
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (xioctl(fd, VIDIOC_STREAMON, &type) < 0)
			errno_exit("VIDIOC_STREAMON");
		break;
	}
}

static void uninit_device(void)
{
	int i;

	switch (io) {
	case IO_METHOD_READ:
		free(buffers[0].start);
		break;
	case V4L2_MEMORY_MMAP:
		for (i = 0; i < n_buffers; ++i)
			if (-1 ==
			    munmap(buffers[i].start, buffers[i].length))
				errno_exit("munmap");
		break;
	case V4L2_MEMORY_USERPTR:
		for (i = 0; i < n_buffers; ++i)
			free(buffers[i].start);
		break;
	}
	free(buffers);
}

static void init_read(unsigned int buffer_size)
{
	buffers = calloc(1, sizeof(*buffers));

	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	buffers[0].length = buffer_size;
	buffers[0].start = malloc(buffer_size);

	if (!buffers[0].start) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}
}

static void init_mmap(void)
{
	struct v4l2_requestbuffers req;

	CLEAR(req);

	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		if (EINVAL == errno) {
			fprintf(stderr, "%s does not support "
				"memory mapping\n", dev_name);
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_REQBUFS");
		}
	}

	if (req.count < 2) {
		fprintf(stderr, "Insufficient buffer memory on %s\n",
			dev_name);
		exit(EXIT_FAILURE);
	}

	buffers = calloc(req.count, sizeof(*buffers));

	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
		struct v4l2_buffer buf;

		CLEAR(buf);

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = n_buffers;

		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
			errno_exit("VIDIOC_QUERYBUF");

		buffers[n_buffers].length = buf.length;
		buffers[n_buffers].start = mmap(NULL /* start anywhere */ ,
						buf.length,
						PROT_READ | PROT_WRITE
						/* required */ ,
						MAP_SHARED
						/* recommended */ ,
						fd, buf.m.offset);

		if (MAP_FAILED == buffers[n_buffers].start)
			errno_exit("mmap");
	}
}

static void init_userp(unsigned int buffer_size)
{
	struct v4l2_requestbuffers req;
	unsigned int page_size;

	page_size = getpagesize();
	buffer_size = (buffer_size + page_size - 1) & ~(page_size - 1);

	CLEAR(req);

	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_USERPTR;

	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		if (EINVAL == errno) {
			fprintf(stderr, "%s does not support "
				"user pointer i/o\n", dev_name);
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_REQBUFS");
		}
	}

	buffers = calloc(4, sizeof(*buffers));
	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for (n_buffers = 0; n_buffers < 4; ++n_buffers) {
		buffers[n_buffers].length = buffer_size;
		buffers[n_buffers].start = memalign( /* boundary */ page_size,
						    buffer_size);

		if (!buffers[n_buffers].start) {
			fprintf(stderr, "Out of memory\n");
			exit(EXIT_FAILURE);
		}
	}
}

static void init_device(int w, int h)
{
	struct v4l2_capability cap;
	int ret;
	int sizeimage;

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		if (EINVAL == errno) {
			fprintf(stderr, "%s is no V4L2 device\n",
				dev_name);
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_QUERYCAP");
		}
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		fprintf(stderr, "%s is no video capture device\n",
			dev_name);
		exit(EXIT_FAILURE);
	}

	switch (io) {
	case IO_METHOD_READ:
		if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
			fprintf(stderr, "%s does not support read i/o\n",
				dev_name);
			exit(EXIT_FAILURE);
		}
		break;
	case V4L2_MEMORY_MMAP:
	case V4L2_MEMORY_USERPTR:
		if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
			fprintf(stderr,
				"%s does not support streaming i/o\n",
				dev_name);
			exit(EXIT_FAILURE);
		}
		break;
	}


//	if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0)
//		perror("get fmt");

	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = w;
	fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
#ifdef WITH_V4L2_LIB
	v4lconvert_data = v4lconvert_create(fd);
	if (v4lconvert_data == NULL)
		errno_exit("v4lconvert_create");
	if (v4lconvert_try_format(v4lconvert_data, &fmt, &src_fmt) != 0)
		errno_exit("v4lconvert_try_format");
	ret = xioctl(fd, VIDIOC_S_FMT, &src_fmt);
	sizeimage = src_fmt.fmt.pix.sizeimage;
	dst_buf = malloc(fmt.fmt.pix.sizeimage);
	printf("raw pixfmt: %c%c%c%c %dx%d\n",
		src_fmt.fmt.pix.pixelformat & 0xff,
	       (src_fmt.fmt.pix.pixelformat >> 8) & 0xff,
	       (src_fmt.fmt.pix.pixelformat >> 16) & 0xff,
	       (src_fmt.fmt.pix.pixelformat >> 24) & 0xff,
		src_fmt.fmt.pix.width, src_fmt.fmt.pix.height);
#else
	ret = xioctl(fd, VIDIOC_S_FMT, &fmt);
	sizeimage = fmt.fmt.pix.sizeimage;
#endif

	if (ret < 0)
		errno_exit("VIDIOC_S_FMT");
// 
//      /* Note VIDIOC_S_FMT may change width and height. */
// 
	printf("pixfmt: %c%c%c%c %dx%d\n",
		fmt.fmt.pix.pixelformat & 0xff,
	       (fmt.fmt.pix.pixelformat >> 8) & 0xff,
	       (fmt.fmt.pix.pixelformat >> 16) & 0xff,
	       (fmt.fmt.pix.pixelformat >> 24) & 0xff,
		fmt.fmt.pix.width, fmt.fmt.pix.height);

	switch (io) {
	case IO_METHOD_READ:
		init_read(sizeimage);
		break;
	case V4L2_MEMORY_MMAP:
		init_mmap();
		break;
	case V4L2_MEMORY_USERPTR:
		init_userp(sizeimage);
		break;
	}
}

static void close_device(void)
{
	close(fd);
}

static int open_device(void)
{
	struct stat st;

	if (stat(dev_name, &st) < 0) {
		fprintf(stderr, "Cannot identify '%s': %d, %s\n",
			dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s is no device\n", dev_name);
		exit(EXIT_FAILURE);
	}

	fd = open(dev_name, O_RDWR /* required */  | O_NONBLOCK, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open '%s': %d, %s\n",
			dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}
	return fd;
}

static void usage(FILE * fp, int argc, char **argv)
{
	fprintf(fp,
		"Usage: %s [options]\n\n"
		"Options:\n"
		"-d | --device name   Video device name [/dev/video0]\n"
		"-f | --format        Image format <width>x<height> [640x480]\n"
		"-g | --grab          Grab an image and exit\n"
		"-h | --help          Print this message\n"
		"-m | --method      m Use memory mapped buffers (default)\n"
		"                   r Use read() calls\n"
		"                   u Use application allocated buffers\n"
		"-r | --raw           No image conversion\n"
		"", argv[0]);
}

static const char short_options[] = "d:f:ghm:r";

static const struct option long_options[] = {
	{"device", required_argument, NULL, 'd'},
	{"format", required_argument, NULL, 'f'},
	{"grab", no_argument, NULL, 'g'},
	{"help", no_argument, NULL, 'h'},
	{"method", required_argument, NULL, 'm'},
	{"raw", no_argument, NULL, 'r'},
	{}
};

int main(int argc, char **argv)
{
	int w, h;

	w = 640;
	h = 480;
	for (;;) {
		int index;
		int c;

		c = getopt_long(argc, argv, short_options, long_options,
				&index);
		if (c < 0)
			break;

		switch (c) {
		case 0:	/* getopt_long() flag */
			break;
		case 'd':
			dev_name = optarg;
			break;
		case 'f':
			if (sscanf(optarg, "%dx%d", &w, &h) != 2) {
				fprintf(stderr, "Invalid image format\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'g':
			grab = 1;
			break;
		case 'h':
			usage(stdout, argc, argv);
			exit(EXIT_SUCCESS);
		case 'm':
			switch (optarg[0]) {
			case 'm':
				io = V4L2_MEMORY_MMAP;
				break;
			case 'r':
				io = IO_METHOD_READ;
				break;
			case 'u':
				io = V4L2_MEMORY_USERPTR;
				break;
			default:
				usage(stderr, argc, argv);
				exit(EXIT_FAILURE);
			}
			break;
		case 'r':
			raw = 1;
			break;
		default:
			usage(stderr, argc, argv);
			exit(EXIT_FAILURE);
		}
	}

	open_device();
	init_device(w, h);
	start_capturing();
#ifdef WITH_GTK
	if (grab)
		get_frame();
	else
		main_frontend(argc, argv);
#else
	mainloop();
#endif
	stop_capturing();
	uninit_device();
	close_device();
	return 0;
}
