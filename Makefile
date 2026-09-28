CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -fPIC -D_FORTIFY_SOURCE=2 -fstack-protector-strong
LIBDIR  ?= $(shell dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null | sed 's|^|/lib/|' || echo /lib)

libnss_llmnr.so.2: nss_llmnr.c
	$(CC) $(CFLAGS) -shared -Wl,-soname,libnss_llmnr.so.2 -o $@ $<

install: libnss_llmnr.so.2
	install -m 0644 libnss_llmnr.so.2 $(DESTDIR)$(LIBDIR)/libnss_llmnr.so.2
	ldconfig

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libnss_llmnr.so.2
	ldconfig

clean:
	rm -f libnss_llmnr.so.2

.PHONY: install uninstall clean
