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
#include <libv4l2.h>
#include <libv4lconvert.h>
#include <glib.h>

#ifdef HAVE_GTK
#include <gtk/gtk.h>

typedef struct __GuiGtk {
	GtkWidget   *drawing_area;
} GuiGtk;

static GuiGtk g_ui;

#endif

#ifdef HAVE_CACA
#include <caca.h>

typedef struct __GuiCaca {
	caca_canvas_t   *cv;
	caca_display_t  *dp;
	caca_dither_t   *im;
	int             ww;
	int             wh;
} GuiCaca;

static GuiCaca c_ui;
#endif

typedef struct __GuiNone {
	long            num_frames;
	long            frame;
	int             grab;
} GuiNone;
static GuiNone n_ui;

typedef void (*GuiUpdateFunction)(unsigned char *pixels, int len);
typedef void (*GuiInitFunction)(int argc, char *argv[], int w, int h, int bpp);

static GuiUpdateFunction    gui_update_function;
static GuiInitFunction      gui_init_function;

static GMainLoop            *loop;

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
	void            *start;
	size_t          length;
};

static char         *dev_name = "/dev/video0";
static int          io = V4L2_MEMORY_MMAP;
static int          fd = -1;
struct buffer       *buffers;
static int          n_buffers;
static struct       v4l2_format fmt;

void gui_none_init(int argc, char *argv[], int w, int h, int bpp)
{

}

void gui_none_update(unsigned char *p, int len)
{

}

#ifdef HAVE_GTK
static void gui_gtk_quit(void)
{
	g_main_loop_quit (loop);
}

void gui_gtk_init(int argc, char *argv[], int w, int h, int bpp)
{
	GtkWidget *window;

	gtk_init(&argc, &argv);

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), PACKAGE_NAME);

	g_signal_connect(G_OBJECT(window), "delete_event",
			   G_CALLBACK(gui_gtk_quit), NULL);
	g_signal_connect(G_OBJECT(window), "destroy",
			   G_CALLBACK(gui_gtk_quit), NULL);
	g_signal_connect(G_OBJECT(window), "key_press_event",
			   G_CALLBACK(gui_gtk_quit), NULL);

	gtk_container_set_border_width(GTK_CONTAINER(window), 2);

	g_ui.drawing_area = gtk_drawing_area_new();
	gtk_drawing_area_size(
			GTK_DRAWING_AREA(g_ui.drawing_area),
			fmt.fmt.pix.width, fmt.fmt.pix.height);

	gtk_container_add(GTK_CONTAINER(window), g_ui.drawing_area);

	gtk_widget_show_all(window);

}

void gui_gtk_update(unsigned char *p, int len)
{
	gdk_draw_rgb_image(
			   gtk_widget_get_window(g_ui.drawing_area),
			   gtk_widget_get_style(g_ui.drawing_area)->white_gc,
			   0, 0,		/* xpos, ypos */
			   fmt.fmt.pix.width, fmt.fmt.pix.height,
			   GDK_RGB_DITHER_NORMAL,
			   p,
			   fmt.fmt.pix.width * 3);
}
#endif

#ifdef HAVE_CACA
void gui_console_update(unsigned char *p, int len)
{
	caca_dither_bitmap(
		c_ui.cv,
		0, 0,
		c_ui.ww, c_ui.wh,
		c_ui.im,
		p);
	caca_refresh_display(c_ui.dp);
}

void gui_console_init(int argc, char *argv[], int w, int h, int bpp)
{
		c_ui.dp = caca_create_display(NULL);
		c_ui.cv = caca_get_canvas(c_ui.dp);
		c_ui.ww = caca_get_canvas_width(c_ui.cv);
		c_ui.wh = caca_get_canvas_height(c_ui.cv);

		caca_set_display_title(c_ui.dp, PACKAGE_NAME);
		c_ui.im = caca_create_dither(
					bpp,
					w, h,
					3 * w /*stride*/,
					0xff0000, 0x00ff00, 0x0000ff, 0);
}
#endif

static void errno_exit(const char *s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

static void process_image(unsigned char *p, int len)
{
	if (n_ui.grab) {
		FILE *f;
		f = fopen("image.dat", "w");
		fwrite(p, 1, len, f);
		fclose(f);
		printf("image dumped to 'image.dat'\n");
	}

	gui_update_function(p, len);

	if (n_ui.num_frames > 0)
		if (++n_ui.frame >= n_ui.num_frames)
			g_main_loop_quit (loop);
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

static void frame_ready(GIOChannel *source, GIOCondition condition, gpointer data)
{
	if (condition & G_IO_IN)
		read_frame();
}

static int get_frame()
{
#if 0
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
#endif
	return read_frame();
}

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
		printf("\tio:\tread\n");
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
#if defined(HAVE_GTK) && defined(HAVE_CACA)
#define UI_AVAIL "gtk,console"
#elif defined(HAVE_GTK)
#define UI_AVAIL "gtk"
#elif defined(HAVE_CACA)
#define UI_AVAIL "console"
#else
#define UI_AVAIL ""
#endif
	fprintf(fp,
		"Usage: %s [options]\n\n"
		"Options:\n"
		"-d | --device name   Video device name [/dev/video0]\n"
		"-s | --size          Image size <width>x<height> [640x480]\n"
		"-g | --grab          Grab an image and exit. Synonym for --frames=1\n"
		"-u | --ui            UI method [none,"UI_AVAIL"]\n"
		"-h | --help          Print this message\n"
		"-n | --frames        Do not show a window, capture n frames [100]\n"
		"-m | --method      m Use memory mapped buffers (default)\n"
		"                   r Use read() calls\n"
		"                   u Use application allocated buffers\n"
		"", argv[0]);
}

static const char short_options[] = "d:f:ghm:rn:u:";

static const struct option long_options[] = {
	{"device", required_argument, NULL, 'd'},
	{"size", required_argument, NULL, 's'},
	{"ui", required_argument, NULL, 'u'},
	{"grab", no_argument, NULL, 'g'},
	{"help", no_argument, NULL, 'h'},
	{"frames", required_argument, NULL, 'n'},
	{"method", required_argument, NULL, 'm'},
	{}
};

int main(int argc, char **argv)
{
	int w;
	int h;
	int bpp;
	GIOChannel *ioc;

	/* default to the gtk interface if available */
	n_ui.frame = 0;
	n_ui.num_frames = 0;
	n_ui.grab = 0;
#ifdef HAVE_GTK
	gui_update_function = gui_gtk_update;
	gui_init_function = gui_gtk_init;
#else
	gui_update_function = gui_none_update;
	gui_init_function = gui_none_init;
#endif

	w = 640;
	h = 480;
	bpp = 24;
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
		case 's':
			if (sscanf(optarg, "%dx%d", &w, &h) != 2) {
				fprintf(stderr, "Invalid image size\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'g':
			n_ui.grab = 1;
			break;
		case 'u':
			if (strcmp(optarg, "none") == 0) {
				gui_update_function = gui_none_update;
				gui_init_function = gui_none_init;
			}
			if (strcmp(optarg, "gtk") == 0) {
#ifdef HAVE_GTK
				gui_update_function = gui_gtk_update;
				gui_init_function = gui_gtk_init;
#else
				fprintf(stderr, "Not compiled with gtk support\n");
				exit(EXIT_FAILURE);
#endif
			}
			if (strcmp(optarg, "console") == 0) {
#ifdef HAVE_CACA
				gui_update_function = gui_console_update;
				gui_init_function = gui_console_init;
#else
				fprintf(stderr, "Not compiled with console support\n");
				exit(EXIT_FAILURE);
#endif
			}
			break;
		case 'n':
			n_ui.num_frames = strtol(optarg, NULL, 10);
			if (n_ui.num_frames <= 0 || errno == EINVAL)
				n_ui.num_frames = DEFAULT_NUM_FRAMES;
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

	if (n_ui.num_frames > 0)
		printf("capturing %ld frames\n", n_ui.num_frames);

	get_frame();

	gui_init_function(argc, argv, w, h, bpp);

	ioc = g_io_channel_unix_new(fd);
	g_io_add_watch(ioc,
			G_IO_IN,
			(GIOFunc)frame_ready,
			NULL);

	loop = g_main_loop_new(NULL, TRUE);
	g_main_loop_run(loop);

	stop_capturing();
	uninit_device();
	close_device();
	return 0;
}
