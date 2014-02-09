#ifndef WAYLAND_BACKEND_H
#define WAYLAND_BACKEND_H

void wayland_backend_init(int argc, char *argv[], int w, int h, int bpp);

void wayland_backend_update(unsigned char *p, int len);

int wayland_backend_get_fd(void);

int wayland_backend_dispatch(void);

#endif // WAYLAND_H
