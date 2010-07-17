/*
 *  Simple V4L2 video viewer.
 *
 *  This program can be used and distributed without restrictions.
 *
 *  http://moinejf.free.fr/
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#ifdef HAVE_GTK
#include <gtk/gtk.h>
#endif

#include <libv4l2.h>
#include <libv4lconvert.h>

/* Sentinal value, must be != V4L2_MEMORY_{MMAP,USERPTR} 
enum v4l2_memory {
        V4L2_MEMORY_MMAP
        V4L2_MEMORY_USERPTR
        V4L2_MEMORY_OVERLAY
};
*/
#define IO_METHOD_READ 42
#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define DEFAULT_NUM_FRAMES 100

struct buffer {
	void *start;
	size_t length;
};

static char *dev_name = "/dev/video0";
static int io = V4L2_MEMORY_MMAP;
static int fd = -1;
struct buffer *buffers;
static int n_buffers;
static struct v4l2_format fmt;
static unsigned int window;
static unsigned int grab;

static void errno_exit(const char *s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

#ifdef HAVE_GTK
static int read_frame(void);

/* graphic functions */
static GtkWidget *drawing_area;

static void frame_ready(GIOChannel *source, GIOCondition condition, gpointer data)
{
	if (condition & G_IO_IN)
		read_frame();
}

static int main_frontend(int argc, char *argv[])
{
	GtkWidget *window;
    GIOChannel *io;

	gtk_init(&argc, &argv);

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), "My WebCam");

	g_signal_connect(G_OBJECT(window), "delete_event",
			   G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(G_OBJECT(window), "destroy",
			   G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(G_OBJECT(window), "key_press_event",
			   G_CALLBACK(gtk_main_quit), NULL);

	gtk_container_set_border_width(GTK_CONTAINER(window), 2);

	drawing_area = gtk_drawing_area_new();
	gtk_drawing_area_size(GTK_DRAWING_AREA(drawing_area),
				  fmt.fmt.pix.width, fmt.fmt.pix.height);

	gtk_container_add(GTK_CONTAINER(window), drawing_area);

	gtk_widget_show_all(window);

    io = g_io_channel_unix_new(fd);
    g_io_add_watch(io,
            G_IO_IN,
            (GIOFunc)frame_ready,
            NULL);

	gtk_main();

	return 0;
}
#endif

static void process_image(unsigned char *p, int len)
{
	if (grab) {
		FILE *f;

		f = fopen("image.dat", "w");
		fwrite(p, 1, len, f);
		fclose(f);
		printf("image dumped to 'image.dat'\n");
		exit(EXIT_SUCCESS);
	}
#ifdef HAVE_GTK
	if (window) {
		gdk_draw_rgb_image(
                   gtk_widget_get_window(drawing_area),
				   gtk_widget_get_style(drawing_area)->white_gc,
				   0, 0,		/* xpos, ypos */
				   fmt.fmt.pix.width, fmt.fmt.pix.height,
				   GDK_RGB_DITHER_NORMAL,
				   p,
				   fmt.fmt.pix.width * 3);
    } else {
		fputc('.', stdout);	fflush(stdout);
    }
#else
	fputc('.', stdout);	fflush(stdout);
#endif
}

static int read_frame(void)
{
	struct v4l2_buffer buf;
	int i;

	switch (io) {
	case IO_METHOD_READ:
		i = v4l2_read(fd, buffers[0].start, buffers[0].length);
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

		if (v4l2_ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
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

		if (v4l2_ioctl(fd, VIDIOC_QBUF, &buf) < 0)
			errno_exit("VIDIOC_QBUF");
		break;
	case V4L2_MEMORY_USERPTR:
		CLEAR(buf);

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_USERPTR;

		if (v4l2_ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
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

		if (v4l2_ioctl(fd, VIDIOC_QBUF, &buf) < 0)
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

#if !WITH_GTK
static void mainloop(int count)
{
	printf("capturing %u frames\n", count);
	while (--count >= 0) {
		for (;;) {
			if (get_frame())
				break;
		}
	}
	fputc('\n', stdout); fflush(stdout);
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

		if (v4l2_ioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
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
		/* Nothing to do. */
		break;
	case V4L2_MEMORY_MMAP:
		for (i = 0; i < n_buffers; ++i) {
			struct v4l2_buffer buf;

			CLEAR(buf);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index = i;

			if (v4l2_ioctl(fd, VIDIOC_QBUF, &buf) < 0)
				errno_exit("VIDIOC_QBUF");
		}

		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (v4l2_ioctl(fd, VIDIOC_STREAMON, &type) < 0)
			errno_exit("VIDIOC_STREAMON");
		break;
	case V4L2_MEMORY_USERPTR:
		for (i = 0; i < n_buffers; ++i) {
			struct v4l2_buffer buf;

			CLEAR(buf);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_USERPTR;
			buf.index = i;
			buf.m.userptr = (unsigned long) buffers[i].start;
			buf.length = buffers[i].length;

			if (v4l2_ioctl(fd, VIDIOC_QBUF, &buf) < 0)
				errno_exit("VIDIOC_QBUF");
		}
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (v4l2_ioctl(fd, VIDIOC_STREAMON, &type) < 0)
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
				v4l2_munmap(buffers[i].start, buffers[i].length))
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

	if (v4l2_ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
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

		if (v4l2_ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
			errno_exit("VIDIOC_QUERYBUF");

		buffers[n_buffers].length = buf.length;
		buffers[n_buffers].start = v4l2_mmap(
						NULL /* start anywhere */ ,
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

	if (v4l2_ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
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
	struct v4lconvert_data *v4lconvert_data;
	struct v4l2_format src_fmt;	 /* raw source format */
	struct v4l2_capability cap;

	if (v4l2_ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
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

	/* libv4l emulates read() on those v4l2 devices that do not support
	it, so this print is just instructional, it should work regardless */
	printf("device capabilities\n\tread:\t%c\n\tstream:\t%c\n",
		(cap.capabilities & V4L2_CAP_READWRITE) ? 'Y' : 'N',
		(cap.capabilities & V4L2_CAP_STREAMING) ? 'Y' : 'N');

	/* set our requested format to V4L2_PIX_FMT_RGB24 */
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = w;
	fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

	/* libv4l also converts mutiple supported formats to V4l2_PIX_FMT_BGR24 or 
	V4l2_PIX_FMT_YUV420, which means the following call should *always* 
	succeed 

	However, we use the libv4lconvert library to print debugging information
	to tell us if libv4l will be doing the conversion internally*/
	v4lconvert_data = v4lconvert_create(fd);
	if (v4lconvert_data == NULL)
		errno_exit("v4lconvert_create");
	if (v4lconvert_try_format(v4lconvert_data, &fmt, &src_fmt) != 0)
		errno_exit("v4lconvert_try_format");

	printf("\tpixfmt:\t%c%c%c%c (%dx%d)\n",
		src_fmt.fmt.pix.pixelformat & 0xff,
		(src_fmt.fmt.pix.pixelformat >> 8) & 0xff,
		(src_fmt.fmt.pix.pixelformat >> 16) & 0xff,
		(src_fmt.fmt.pix.pixelformat >> 24) & 0xff,
		src_fmt.fmt.pix.width, src_fmt.fmt.pix.height);

	printf("application\n\tconv:\t%c\n", 
		v4lconvert_needs_conversion(v4lconvert_data,
			&src_fmt,
			&fmt) ? 'Y' : 'N');

	v4lconvert_destroy(v4lconvert_data);

	/* Actually set the pixfmt so that libv4l uses its conversion magic */
	if (v4l2_ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
		errno_exit("VIDIOC_S_FMT");

	printf("\tpixfmt:\t%c%c%c%c (%dx%d)\n",
		fmt.fmt.pix.pixelformat & 0xff,
		(fmt.fmt.pix.pixelformat >> 8) & 0xff,
		(fmt.fmt.pix.pixelformat >> 16) & 0xff,
		(fmt.fmt.pix.pixelformat >> 24) & 0xff,
		fmt.fmt.pix.width, fmt.fmt.pix.height);

	switch (io) {
	case IO_METHOD_READ:
		printf("\tio:\tio\n");
		init_read(fmt.fmt.pix.sizeimage);
		break;
	case V4L2_MEMORY_MMAP:
		printf("\tio:\tmmap\n");
		init_mmap();
		break;
	case V4L2_MEMORY_USERPTR:
		printf("\tio:\tusrptr\n");
		init_userp(fmt.fmt.pix.sizeimage);
		break;
	}
}

static void close_device(void)
{
	v4l2_close(fd);
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

	fd = v4l2_open(dev_name, O_RDWR /* required */  | O_NONBLOCK, 0);
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
        "-n | --frames        Do not show a window, capture frame [100]\n"
		"-m | --method      m Use memory mapped buffers (default)\n"
		"                   r Use read() calls\n"
		"                   u Use application allocated buffers\n"
		"", argv[0]);
}

static const char short_options[] = "d:f:ghm:rn:";

static const struct option long_options[] = {
	{"device", required_argument, NULL, 'd'},
	{"format", required_argument, NULL, 'f'},
	{"grab", no_argument, NULL, 'g'},
	{"help", no_argument, NULL, 'h'},
	{"frames", required_argument, NULL, 'n'},
	{"method", required_argument, NULL, 'm'},
	{}
};

int main(int argc, char **argv)
{
	int w, h;
    long frames;

    frames = DEFAULT_NUM_FRAMES;
	grab = 0;
	window = 1;
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
		case 'n':
            window = 0;
			frames = strtol(optarg, NULL, 10);
            if (frames <= 0 || errno == EINVAL)
                frames = DEFAULT_NUM_FRAMES;
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
		default:
			usage(stderr, argc, argv);
			exit(EXIT_FAILURE);
		}
	}

	open_device();
	init_device(w, h);
	start_capturing();
#ifdef HAVE_GTK
	if (window) {
		if (grab) {
			printf("\tgtk:\tno\n");
			get_frame();
		} else {
			printf("\tgtk:\tyes\n");
			main_frontend(argc, argv);
		}
	} else {
		printf("\tgtk:\tdisabled\n");
		mainloop(frames);
	}
#else
	printf("\tgtk:\tdisabled\n");
	mainloop(frames);
#endif
	stop_capturing();
	uninit_device();
	close_device();
	return 0;
}
