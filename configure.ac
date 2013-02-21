AC_PREREQ(2.59)
AC_INIT(Simple Video Viewer, 1.0, john.stowers@gmail.com)
AM_INIT_AUTOMAKE([1.9])
AC_CONFIG_SRCDIR([svv.c])
AC_PROG_CC

#compulsory
PKG_CHECK_MODULES(LIBV4L, libv4l2)
PKG_CHECK_MODULES(LIBV4LCONVERT, libv4lconvert)
PKG_CHECK_MODULES(GLIB, glib-2.0)

#gtk+ is optional
PKG_CHECK_MODULES(GTK, gtk+-2.0, 
                  [
                    have_gtk=yes
                    AC_DEFINE(HAVE_GTK,1,[GTK+ 2.x toolkit])
                  ],
                  [ have_gtk=no ]
)

#caca is optional
PKG_CHECK_MODULES(CACA, caca > 0.99, 
                  [
                    have_caca=yes
                    AC_DEFINE(HAVE_CACA,1,[libcaca ascii toolkit])
                  ],
                  [ have_caca=no ]
)


AC_CONFIG_FILES([Makefile])
AC_OUTPUT

AC_MSG_RESULT([
    ${PACKAGE_NAME} v ${PACKAGE_VERSION}

    Install path:             ${prefix}
    Use gtk+ 2.x:             ${have_gtk}
    Use libcaca:              ${have_caca}
])

