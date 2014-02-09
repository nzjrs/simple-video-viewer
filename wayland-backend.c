#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include <wayland-client.h>

#include "wayland-backend.h"

#define cm_container_of(ptr, type, member) ({					\
	const __typeof__( ((type *)0)->member ) *__mptr = (ptr);		\
	(type *)( (char *)__mptr - offsetof(type,member) );})

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shell *shell;
	struct wl_shm *shm;
	uint32_t formats;

	int display_fd;
};

struct buffer {
	struct wl_buffer *buffer;
	void *shm_data;
	int busy;
};

struct window {
	struct display *display;
	int width, height;
	struct wl_surface *surface;
	struct wl_shell_surface *shell_surface;
	struct buffer buffers[2];

	struct wl_callback *callback;

	int frame_ready;
};

static struct display *s_display;
static struct window *s_window;

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
	struct display *d = data;

	d->formats |= (1 << format);
}

struct wl_shm_listener shm_listener = {
	shm_format
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
					   uint32_t id, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
				wl_registry_bind(registry, id,
								 &wl_compositor_interface, 1);
	} else if (strcmp(interface, "wl_shell") == 0) {
		d->shell = wl_registry_bind(registry,
									id, &wl_shell_interface, 1);
	} else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = wl_registry_bind(registry, id,
								  &wl_shm_interface, 1);

		wl_shm_add_listener(d->shm, &shm_listener, d);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
							  uint32_t name)
{

}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = data;

	mybuf->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static int
set_cloexec_or_close(int fd)
{
	long flags;

	if (fd == -1)
		return -1;

	flags = fcntl(fd, F_GETFD);
	if (flags == -1)
		goto err;

	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
		goto err;

	return fd;

err:
	close(fd);
	return -1;
}

static int
create_tmpfile_cloexec(char *tmpname)
{
	int fd;

#ifdef HAVE_MKOSTEMP
	fd = mkostemp(tmpname, O_CLOEXEC);
	if (fd >= 0)
		unlink(tmpname);
#else
	fd = mkstemp(tmpname);
	if (fd >= 0) {
		fd = set_cloexec_or_close(fd);
		unlink(tmpname);
	}
#endif

	return fd;
}

static int
os_create_anonymous_file(off_t size)
{
	static const char tpl[] = "/weston-shared-XXXXXX";
	const char *path;
	char *name;
	int fd;
	int ret;

	path = getenv("XDG_RUNTIME_DIR");
	if (!path) {
		errno = ENOENT;
		return -1;
	}

	name = malloc(strlen(path) + sizeof(tpl));
	if (!name)
		return -1;

	strcpy(name, path);
	strcat(name, tpl);

	fd = create_tmpfile_cloexec(name);

	free(name);

	if (fd < 0)
		return -1;

#ifdef HAVE_POSIX_FALLOCATE
	ret = posix_fallocate(fd, 0, size);
	if (ret != 0) {
		close(fd);
		errno = ret;
		return -1;
	}
#else
	ret = ftruncate(fd, size);
	if (ret < 0) {
		close(fd);
		return -1;
	}
#endif

	return fd;
}

static int
create_shm_buffer(struct display *display, struct buffer *buffer,
				  int width, int height, uint32_t format)
{
	struct wl_shm_pool *pool;
	int fd, stride, size;
	void *data;

	stride = width * 4;
	size = stride * height;

	fd = os_create_anonymous_file(size);

	if (fd < 0) {
		fprintf(stderr, "failed to create anonymous file of size %d\n",
				size);
		return -1;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE,
				MAP_SHARED, fd, 0);

	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap failed %m\n");
		close(fd);
		return -1;
	}

	pool = wl_shm_create_pool(display->shm, fd, size);
	buffer->buffer = wl_shm_pool_create_buffer(pool, 0,
											   width, height,
											   stride, format);

	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
	wl_shm_pool_destroy(pool);
	close(fd);

	buffer->shm_data = data;

	return 0;
}

static struct buffer *
window_next_buffer(struct window *window)
{
	struct buffer *buffer;
	int ret = 0;

	if (!window->buffers[0].busy) {
		buffer = &window->buffers[0];
	} else if (!window->buffers[1].busy) {
		buffer = &window->buffers[1];
	} else {
		return NULL;
	}

	if (!buffer->buffer) {
		ret = create_shm_buffer(window->display, buffer,
								window->width, window->height,
								WL_SHM_FORMAT_XRGB8888);

		if (ret < 0) {
			return NULL;
		}

		memset(buffer->shm_data, 0xff,
			   window->width * window->height * 4);
	}

	return buffer;
}

static void
handle_ping(void *data, struct wl_shell_surface *shell_surface,
			uint32_t serial)
{
	wl_shell_surface_pong(shell_surface, serial);
}

static void
handle_configure(void *data, struct wl_shell_surface *shell_surface,
				 uint32_t edges, int32_t width, int32_t height)
{
}

static void
handle_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	handle_ping,
	handle_configure,
	handle_popup_done
};

static const struct wl_callback_listener frame_listener;

static int
conv_rgb24_to_rgb32(unsigned char *src, unsigned char *dest, int size_src)
{
	int i = 0;

	for (i = 0; i < size_src; i += 3) {
		*dest++ = *(src + 2);
		*dest++ = *(src + 1);
		*dest++ = *(src);
		*dest++ = 0;

		src += 3;
	}
}

static void
handle_wayland_ready(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;

	window->frame_ready = 1;

	if (callback)
		wl_callback_destroy(callback);

}

static const struct wl_callback_listener frame_listener = {
	handle_wayland_ready
};

static struct window *
create_window(struct display *display, int width, int height)
{
	struct window *window;

	window = calloc(1, sizeof *window);

	if (window == NULL)
		return NULL;

	window->display = display;
	window->width = width;
	window->height = height;

	window->surface = wl_compositor_create_surface(display->compositor);
	window->shell_surface = wl_shell_get_shell_surface(display->shell,
													   window->surface);

	if (window->shell_surface)
		wl_shell_surface_add_listener(window->shell_surface,
									  &shell_surface_listener, window);

	wl_shell_surface_set_toplevel(window->shell_surface);

	window->frame_ready = 1;

	return window;
}

static struct display *
create_display(void)
{
	struct display *display;

	display = malloc(sizeof *display);

	if (display == NULL) {
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}

	display->display = wl_display_connect(NULL);
	assert(display->display);

	display->formats = 0;
	display->registry = wl_display_get_registry(display->display);

	display->display_fd = wl_display_get_fd(display->display);

	wl_registry_add_listener(display->registry,
							 &registry_listener, display);

	wl_display_roundtrip(display->display);

	if (display->shm == NULL) {
		fprintf(stderr, "No wl_shm global\n");
		exit(1);
	}

	wl_display_roundtrip(display->display);

	return display;
}

static void
destroy_window(struct window *window)
{
	if (window->shell_surface)
		wl_shell_surface_destroy(window->shell_surface);

	if (window->surface)
		wl_surface_destroy(window->surface);

	free(window);
}

static void
destroy_display(struct display *display)
{
	if (display->shm)
		wl_shm_destroy(display->shm);

	if (display->shell)
		wl_shell_destroy(display->shell);

	if (display->compositor)
		wl_compositor_destroy(display->compositor);

	if (display->registry)
		wl_registry_destroy(display->registry);

	wl_display_flush(display->display);
	wl_display_disconnect(display->display);

	free(display);
}

static void
display_exit(struct display *disp)
{
}

void
wayland_backend_init(int argc, char *argv[], int w, int h, int bpp)
{
	s_display = create_display();
	s_window = create_window(s_display, w, h);
}

void
wayland_backend_update(unsigned char *p, int len)
{
	struct buffer *buffer;

	if (s_window->frame_ready == 0) {
		return;
	}

	buffer = window_next_buffer(s_window);

	if (!buffer) {
		return;
	}

	/** convert to wayland shm format */
	conv_rgb24_to_rgb32(p, buffer->shm_data, len);

	wl_surface_attach(s_window->surface, buffer->buffer, 0, 0);
	wl_surface_damage(s_window->surface,
					  0, 0, s_window->width, s_window->height);

	s_window->callback = wl_surface_frame(s_window->surface);
	wl_callback_add_listener(s_window->callback, &frame_listener, s_window);
	wl_surface_commit(s_window->surface);
	buffer->busy = 1;

	s_window->frame_ready = 0;

	wl_display_flush(s_display->display);
}

int
wayland_backend_get_fd()
{
	return s_display->display_fd;
}

int wayland_backend_dispatch()
{
	return wl_display_dispatch(s_display->display);
}
