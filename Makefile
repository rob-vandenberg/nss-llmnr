CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -fPIC -D_FORTIFY_SOURCE=2 -fstack-protector-strong

# Directory where glibc looks for NSS modules:
#   Debian/Ubuntu (multiarch):  /lib/x86_64-linux-gnu, /lib/aarch64-linux-gnu, ...
#   Red Hat/Fedora/SUSE (64-bit): /usr/lib64
#   Others (Arch, 32-bit):       /usr/lib
# Override with: make install LIBDIR=/path
MULTIARCH := $(shell $(CC) -print-multiarch 2>/dev/null)
ifneq ($(MULTIARCH),)
LIBDIR  ?= /lib/$(MULTIARCH)
else ifneq ($(wildcard /usr/lib64/libc.so.6),)
LIBDIR  ?= /usr/lib64
else
LIBDIR  ?= /usr/lib
endif

libnss_llmnr.so.2: nss_llmnr.c
	$(CC) $(CFLAGS) -shared -Wl,-soname,libnss_llmnr.so.2 -o $@ $<

install: libnss_llmnr.so.2
	install -D -m 0644 libnss_llmnr.so.2 $(DESTDIR)$(LIBDIR)/libnss_llmnr.so.2
	ldconfig

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libnss_llmnr.so.2
	ldconfig

clean:
	rm -f libnss_llmnr.so.2

.PHONY: install uninstall clean
