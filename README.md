# nss-llmnr

A glibc NSS (Name Service Switch) module that resolves Windows host names on the local network using LLMNR ([RFC 4795](https://www.rfc-editor.org/rfc/rfc4795)). It resolves them the way Windows does: it queries on every network interface, collects all answers, and returns the most reachable address first.

## Background

Windows computers use LLMNR (Link-Local Multicast Name Resolution) to find each other by name on a local network when no DNS entry exists. A Windows machine with more than one network interface, for example Ethernet plus Wi-Fi, or a virtual or VPN adapter, answers an LLMNR query once for each interface. The client therefore receives several different addresses for the same name.

Windows handles this by waiting for all answers and choosing the most suitable address. systemd-resolved, the default resolver on most Linux distributions, uses whichever answer arrives first. For a multihomed host, that is effectively a random address, often one on a network you cannot reach. Connections then fail intermittently, and file managers such as Nemo or Dolphin cannot open network shares by name.

nss-llmnr brings the Windows behaviour to Linux.

## How it works

1. The module handles single-label names only (`winpc01`, not `winpc01.example.com`).
2. It sends the LLMNR query on every active IPv4 interface.
3. After the first answer arrives, it keeps listening for another 250 ms to collect answers from the host's other interfaces.
4. It sorts the collected addresses and returns them in this order:

| Priority | Address                                                          |
|----------|------------------------------------------------------------------|
| 1        | Addresses in the same subnet as one of your own interfaces       |
| 2        | Other private addresses (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`) |
| 3        | All other addresses                                              |

If there is no answer, the query is retried up to 3 times with a 300 ms timeout each. After that, the module reports the name as not found, and the next source listed in `nsswitch.conf` (such as DNS) takes over.

## Requirements

1. Linux with glibc
2. A C compiler and `make`
3. An IPv4 network

On Debian, Ubuntu and derivatives, install the build tools with:

```sh
sudo apt install build-essential
```

## Installation

Extract the archive, build, and install:

```sh
tar xzf nss-llmnr.tar.gz
cd nss-llmnr
make
sudo make install
```

Installing the module does not activate it. Continue with Configuration.

## Configuration

glibc decides which sources to use for host name lookups from the `hosts` line in `/etc/nsswitch.conf`. To activate nss-llmnr, add `llmnr` to that line.

First make a backup:

```sh
sudo cp /etc/nsswitch.conf /etc/nsswitch.conf.before-llmnr
```

Then open the file as root in a text editor, for example:

```sh
sudo nano /etc/nsswitch.conf
```

A typical file looks like this (the exact contents vary per distribution):

```
# /etc/nsswitch.conf
#
# Example configuration of GNU Name Service Switch functionality.
# If you have the `glibc-doc-reference' and `info' packages installed, try:
# `info libc "Name Service Switch"' for information about this file.

passwd:         files systemd
group:          files systemd
shadow:         files systemd
gshadow:        files systemd

hosts:          files mdns4_minimal [NOTFOUND=return] dns
networks:       files

protocols:      db files
services:       db files
ethers:         db files
rpc:            db files

netgroup:       nis
```

Find the line that starts with `hosts:`:

```
hosts:          files mdns4_minimal [NOTFOUND=return] dns
```

Add `llmnr` directly after `files`:

```
hosts:          files llmnr mdns4_minimal [NOTFOUND=return] dns
```

Leave the rest of the line unchanged. Placing `llmnr` directly after `files` ensures that entries in `/etc/hosts` still take precedence, and that nss-llmnr answers before any other source that might return an unreachable address.

Save the file. The change applies to programs started after the edit. Restart programs that were already running, such as your file manager.

## Verifying

Use `getent`, which resolves names through NSS the same way applications do. Replace `winpc01` with the name of a Windows computer on your network:

```sh
getent ahostsv4 winpc01
```

Example output:

```
192.168.1.50    STREAM winpc01
192.168.1.50    DGRAM
192.168.1.50    RAW
172.20.0.5      STREAM
172.20.0.5      DGRAM
172.20.0.5      RAW
```

The first address listed is the one applications will use. It should be the address in your own subnet.

A quick connectivity check:

```sh
ping -c 2 winpc01
```

Note that `dig`, `host` and `nslookup` query DNS servers directly and bypass NSS. They do not use this module, so do not use them to test it.

## Uninstalling

First deactivate the module. Open `/etc/nsswitch.conf` as root and remove `llmnr` from the `hosts` line, so it looks as it did before:

```
hosts:          files mdns4_minimal [NOTFOUND=return] dns
```

Alternatively, restore the backup you made during configuration:

```sh
sudo cp /etc/nsswitch.conf.before-llmnr /etc/nsswitch.conf
```

Then remove the module from the source directory:

```sh
sudo make uninstall
```

## Limitations

1. IPv4 only. IPv6 LLMNR queries and answers are not supported.
2. Single-label names only. Fully qualified names are passed on to the next source in `nsswitch.conf`.
3. A successful lookup takes at least 250 ms longer than the first answer, because the module waits for answers from all of the host's interfaces.
4. An unknown name delays the lookup by up to about 900 ms (3 × 300 ms) before the next source is tried.

## References

1. [RFC 4795: Link-Local Multicast Name Resolution (LLMNR)](https://www.rfc-editor.org/rfc/rfc4795)
2. `man 5 nsswitch.conf`
3. `info libc "Name Service Switch"`
