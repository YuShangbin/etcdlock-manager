# Copyright 2025 EasyStack, Inc.
# Copyright 2010-2011 Red Hat, Inc.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v2 or (at your option) any later version.

include ../common.mk

CMD_TARGET = etcdlock_manager
HEADER_TARGET = etcdlock_manager_rv.h
MAN_TARGET = etcdlock_manager.8

SOMAJOR=1
SOMINOR=0

LIB_ENTIRE_TARGET = libetcdlock_manager
LIB_CLIENT_TARGET = libetcdlock_manager_client
LIBPC_ENTIRE_TARGET = libetcdlock_manager.pc
LIBPC_CLIENT_TARGET = libetcdlock_manager_client.pc
LIBSO_ENTIRE_TARGET = $(LIB_ENTIRE_TARGET).so.$(SOMAJOR).$(SOMINOR)
LIBSO_CLIENT_TARGET = $(LIB_CLIENT_TARGET).so.$(SOMAJOR).$(SOMINOR)

CMD_SOURCE = \
	etcdlock.c \
	libvirt_helper.c \
	lockfile.c \
	log.c \
	main.c \
	timeouts.c \
	watchdog.c \
	monotime.c \
	cmd.c \
	etcdlock_manager_sock.c \
	env.c

LIB_ENTIRE_SOURCE = \
	client.c \
	etcdlock_manager_sock.c \
	etcdlock.c \
	timeouts.c \
	monotime.c \
	env.c

LIB_CLIENT_SOURCE = \
	client.c \
	etcdlock_manager_sock.c \
	env.c

LIBPC_ENTIRE_SOURCE = libetcdlock_manager.pc.in
LIBPC_CLIENT_SOURCE = libetcdlock_manager_client.pc.in

VER=$(shell cat ../VERSION)
CFLAGS += -DVERSION=\"$(VER)\"

CMD_CFLAGS = $(CFLAGS) -fPIE -DPIE
LIB_ENTIRE_CFLAGS = $(CFLAGS) -fPIC
LIB_CLIENT_CFLAGS = $(CFLAGS) -fPIC

CMD_LDFLAGS = $(LDFLAGS) -Wl,-z,relro -pie
LIB_ENTIRE_LDFLAGS = $(LDFLAGS) -Wl,-z,relro -pie
LIB_CLIENT_LDFLAGS = $(LDFLAGS) -Wl,-z,relro -pie

CMD_LDADD = -lpthread -lrt -lcurl -letcdlock_manager -L../wdmd -lwdmd
LIB_ENTIRE_LDADD = -lpthread -lrt -lcurl -L../wdmd -lwdmd

all: $(LIBSO_ENTIRE_TARGET) $(LIBSO_CLIENT_TARGET) $(CMD_TARGET) $(LIBPC_ENTIRE_TARGET) $(LIBPC_CLIENT_TARGET)

$(LIBSO_ENTIRE_TARGET): $(LIB_ENTIRE_SOURCE)
	$(CC) $(LIB_ENTIRE_CFLAGS) $(LIB_ENTIRE_LDFLAGS) -shared -o $@ -Wl,-soname=$(LIB_ENTIRE_TARGET).so.$(SOMAJOR) $^ $(LIB_ENTIRE_LDADD)
	ln -sf $(LIBSO_ENTIRE_TARGET) $(LIB_ENTIRE_TARGET).so
	ln -sf $(LIBSO_ENTIRE_TARGET) $(LIB_ENTIRE_TARGET).so.$(SOMAJOR)

$(LIBSO_CLIENT_TARGET): $(LIB_CLIENT_SOURCE)
	$(CC) $(LIB_CLIENT_CFLAGS) $(LIB_CLIENT_LDFLAGS) -shared -o $@ -Wl,-soname=$(LIB_CLIENT_TARGET).so.$(SOMAJOR) $^
	ln -sf $(LIBSO_CLIENT_TARGET) $(LIB_CLIENT_TARGET).so
	ln -sf $(LIBSO_CLIENT_TARGET) $(LIB_CLIENT_TARGET).so.$(SOMAJOR)

$(CMD_TARGET): $(LIBSO_ENTIRE_TARGET) $(CMD_SOURCE)
	$(CC) $(CMD_CFLAGS) $(CMD_LDFLAGS) $(CMD_SOURCE) $(CMD_LDADD) -o $@ -L.

$(LIBPC_ENTIRE_TARGET): $(LIBPC_ENTIRE_SOURCE)
	sed -e "s/@VERSION@/$(VER)/" $(LIBPC_ENTIRE_SOURCE) > $(LIBPC_ENTIRE_TARGET)

$(LIBPC_CLIENT_TARGET): $(LIBPC_CLIENT_SOURCE)
	sed -e "s/@VERSION@/$(VER)/" $(LIBPC_CLIENT_SOURCE) > $(LIBPC_CLIENT_TARGET)

clean:
	rm -f *.o *.so *.so.* $(CMD_TARGET) $(LIBSO_ENTIRE_TARGET) $(LIBSO_CLIENT_TARGET) $(LIBPC_ENTIRE_TARGET) $(LIBPC_CLIENT_TARGET)

INSTALL=install

DESTDIR=
BINDIR=/usr/sbin
LIBDIR=/usr/lib64
HEADIR=/usr/include
MANDIR=/usr/share/man

.PHONY: install
install: all
	$(INSTALL) -d $(DESTDIR)/$(BINDIR)
	$(INSTALL) -d $(DESTDIR)/$(LIBDIR)
	$(INSTALL) -d $(DESTDIR)/$(HEADIR)
	$(INSTALL) -d $(DESTDIR)/$(MANDIR)/man8
	$(INSTALL) -d $(DESTDIR)/$(LIBDIR)/pkgconfig
	$(INSTALL) -c -m 755 $(CMD_TARGET) $(DESTDIR)/$(BINDIR)
	$(INSTALL) -c -m 755 $(LIBSO_ENTIRE_TARGET) $(DESTDIR)/$(LIBDIR)
	$(INSTALL) -c -m 755 $(LIBSO_CLIENT_TARGET) $(DESTDIR)/$(LIBDIR)
	$(INSTALL) -c -m 644 $(LIBPC_ENTIRE_TARGET) $(DESTDIR)/$(LIBDIR)/pkgconfig
	$(INSTALL) -c -m 644 $(LIBPC_CLIENT_TARGET) $(DESTDIR)/$(LIBDIR)/pkgconfig
	cp -a $(LIB_ENTIRE_TARGET).so $(DESTDIR)/$(LIBDIR)
	cp -a $(LIB_CLIENT_TARGET).so $(DESTDIR)/$(LIBDIR)
	cp -a $(LIB_ENTIRE_TARGET).so.$(SOMAJOR) $(DESTDIR)/$(LIBDIR)
	cp -a $(LIB_CLIENT_TARGET).so.$(SOMAJOR) $(DESTDIR)/$(LIBDIR)
	$(INSTALL) -c -m 644 $(HEADER_TARGET) $(DESTDIR)/$(HEADIR)
	$(INSTALL) -m 644 $(MAN_TARGET) $(DESTDIR)/$(MANDIR)/man8/
