AC_PREREQ(2.59)
AC_INIT(helloworld, 0.0.1, dev@helloworld.org)
AM_INIT_AUTOMAKE([1.9])
AC_CONFIG_SRCDIR([svv.c])
AC_PROG_CC

#libv4l is compulsory
PKG_CHECK_MODULES(LIBV4L, libv4l1 libv4l2)
#gtk+ is optional
PKG_CHECK_MODULES(GTK, gtk+-2.0, 
                  [
                    have_gtk=yes
                    AC_DEFINE(HAVE_GTK,,[GTK+ 2.x toolkit])
                  ],
                  [ have_gtk=no ]
)

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

AC_MSG_RESULT([
    Install path:             ${prefix}
    Use gtk+ 2.x:             ${have_gtk}
])

