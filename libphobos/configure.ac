# Process this file with autoconf to produce a configure script.
# Copyright (C) 2006-2018 Free Software Foundation, Inc.
#
# GCC is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# GCC is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with GCC; see the file COPYING3.  If not see
# <http://www.gnu.org/licenses/>.

# This requires that you have your environment set-up to use explicit
# versions of automake and autoconf.
#
#    export ACLOCAL=/usr/bin/aclocal-1.11
#    export AUTOMAKE=/usr/bin/automake-1.11
#    export AUTOM4TE=/usr/bin/autom4te2.64
#    export AUTOCONF=/usr/bin/autoconf2.64
#
#    autoreconf2.64
#

AC_PREREQ(2.64)
AC_INIT(package-unused, version-unused,, libphobos)
AC_CONFIG_SRCDIR(libdruntime/gcc/attribute.d)
AC_CONFIG_HEADERS(config.h)

AM_ENABLE_MULTILIB(, ..)
AC_CANONICAL_SYSTEM

target_alias=${target_alias-$target}
AC_SUBST(target_alias)

# 1.11.1: Require that version of automake.
# foreign: Don't require README, INSTALL, NEWS, etc.
# no-define: Don't define PACKAGE and VERSION.
# no-dependencies: Don't generate automatic dependencies.
#    (because it breaks when using bootstrap-lean, since some of the
#    headers are gone at "make install" time).
# -Wall: Issue all automake warnings.
# -Wno-portability: Don't warn about constructs supported by GNU make.
#    (because GCC requires GNU make anyhow).
AM_INIT_AUTOMAKE([1.11.1 foreign no-dist no-define no-dependencies -Wall -Wno-portability])

m4_rename([_AC_ARG_VAR_PRECIOUS],[glibd_PRECIOUS])
m4_define([_AC_ARG_VAR_PRECIOUS],[])
AM_PROG_AS
AC_PROG_CC
AC_PROG_GDC
WITH_LOCAL_DRUNTIME([GDC_CHECK_COMPILE], [])

m4_rename_force([glibd_PRECIOUS],[_AC_ARG_VAR_PRECIOUS])

AC_SUBST(CFLAGS)
AM_MAINTAINER_MODE
AC_PROG_RANLIB
AC_PROG_INSTALL
AC_PROG_MAKE_SET

# This should be inherited in the recursive make, but ensure it is defined.
test "$AR" || AR=ar
AC_SUBST(AR)

CC_FOR_BUILD=${CC_FOR_BUILD:-gcc}
AC_SUBST(CC_FOR_BUILD)
AC_SUBST(CFLAGS_FOR_BUILD)

# Enable libtool
LT_INIT(dlopen)
AM_PROG_LIBTOOL
WITH_LOCAL_DRUNTIME([LT_LANG([D])], [])
AM_CONDITIONAL([ENABLE_SHARED], [test "$enable_shared" = "yes"])
AM_CONDITIONAL([ENABLE_STATIC], [test "$enable_static" = "yes"])

# libtool variables for Phobos shared and position-independent compiles.
#
# Use phobos_compiler_shared_flag to designate the compile-time flags for
# creating shared objects. Default: -fversion=Shared.
#
# Use phobos_compiler_pic_flag to designate the compile-time flags for
# creating position-independent objects. This varies with the target
# hardware and operating system, but is often: -fPIC.
#
# The distinction between pic and shared compilation flags is not present in
# libtool, and so we make it here.  How it is handled is that in shared
# compilations the `lt_prog_compiler_pic_D' variable is used to instead
# ensure that conditional compilation of shared runtime code is compiled in.
# The original PIC flags are then used in the compilation of every object.
#
# Why are objects destined for libgphobos.a compiled with -fPIC?
# Because -fPIC is not harmful to use for objects destined for static
# libraries. In addition, using -fPIC will allow the use of static
# libgphobos.a in the creation of other D shared libraries.
if test "$enable_shared" = yes; then
  phobos_compiler_pic_flag="$lt_prog_compiler_pic_D"
  phobos_compiler_shared_flag="-fversion=Shared"
else
  phobos_compiler_pic_flag=
  phobos_compiler_shared_flag=
fi
AC_SUBST(phobos_compiler_pic_flag)
AC_SUBST(phobos_compiler_shared_flag)

# Override the libtool's pic_flag and pic_mode.
# Do this step after AM_PROG_LIBTOOL, but before AC_OUTPUT.
# NB: this impacts --with-pic and --without-pic.
lt_prog_compiler_pic_D="$phobos_compiler_shared_flag"
pic_mode='default'

# Determine what GCC version number to use in filesystem paths.
GCC_BASE_VER

# libphobos/libdruntime specific options and feature detection
DRUNTIME_CONFIGURE
DRUNTIME_MULTILIB
DRUNTIME_WERROR
DRUNTIME_GC
DRUNTIME_OS_UNIX
DRUNTIME_OS_SOURCES
DRUNTIME_OS_THREAD_MODEL
DRUNTIME_OS_ARM_EABI_UNWINDER
DRUNTIME_OS_MINFO_BRACKETING

WITH_LOCAL_DRUNTIME([
  AC_LANG_PUSH([D])
  AC_SEARCH_LIBS([malloc], [c])
  DRUNTIME_LIBRARIES_THREAD
  AC_SEARCH_LIBS([cosf], [m])
  AC_SEARCH_LIBS([clock_gettime], [rt])
  DRUNTIME_ENABLE_ATOMIC_BUILTINS
  AC_LANG_POP([D])
], [-nophoboslib])

DRUNTIME_LIBRARIES_ATOMIC
DRUNTIME_LIBRARIES_BACKTRACE
DRUNTIME_LIBRARIES_DLOPEN
DRUNTIME_LIBRARIES_ZLIB
DRUNTIME_INSTALL_DIRECTORIES

# Add dependencies for libgphobos.spec file
SPEC_PHOBOS_DEPS="$LIBS"
AC_SUBST(SPEC_PHOBOS_DEPS)

# Libdruntime / phobos soname version
DRUNTIME_SOVERSION="83:0:0"
PHOBOS_SOVERSION="83:0:0"
AC_SUBST([DRUNTIME_SOVERSION])
AC_SUBST([PHOBOS_SOVERSION])

# Set default flags (after DRUNTIME_WERROR!)
if test -z "$GDCFLAGS"; then
    GDCFLAGS="-Wall $WERROR_FLAG -g -frelease -O2"
fi
AC_SUBST(GDCFLAGS)

if test -z "$GDCFLAGSX"; then
    GDCFLAGSX="-Wall $WERROR_FLAG -g -fno-release -funittest"
fi
AC_SUBST(GDCFLAGSX)

# Sanity check for the cross-compilation case:
AC_CHECK_HEADER(stdio.h,:,
  [AC_MSG_ERROR([cannot find stdio.h.])])

AC_CONFIG_FILES(Makefile src/Makefile libdruntime/Makefile testsuite/Makefile)

AC_CONFIG_FILES(libdruntime/gcc/config.d libdruntime/gcc/libbacktrace.d)
AC_CONFIG_FILES(src/libgphobos.spec)
AC_CONFIG_FILES([testsuite/testsuite_flags],[chmod +x testsuite/testsuite_flags])

# We need multilib support, but only if configuring for the target.
AC_CONFIG_COMMANDS([default],
[if test -n "$CONFIG_FILES"; then
   if test -n "${with_target_subdir}"; then
     # Multilibs need MULTISUBDIR defined correctly in certain makefiles so
     # that multilib installs will end up installed in the correct place.
     # The testsuite needs it for multilib-aware ABI baseline files.
     # To work around this not being passed down from config-ml.in ->
     # srcdir/Makefile.am -> srcdir/{src,libdruntime,...}/Makefile.am, manually
     # append it here.  Only modify Makefiles that have just been created.
     #
     # Also, get rid of this simulated-VPATH thing that automake does.
     cat > vpsed << \_EOF
  s!`test -f '$<' || echo '$(srcdir)/'`!!
_EOF
     for i in $SUBDIRS; do
      case $CONFIG_FILES in
       *${i}/Makefile*)
	 #echo "Adding MULTISUBDIR to $i/Makefile"
	 sed -f vpsed $i/Makefile > tmp
	 grep '^MULTISUBDIR =' Makefile >> tmp
	 mv tmp $i/Makefile
	 ;;
      esac
     done
     rm vpsed
   fi
 fi
],
[
# Variables needed in config.status (file generation) which aren't already
# passed by autoconf.
SUBDIRS="$SUBDIRS"
])

AC_OUTPUT
