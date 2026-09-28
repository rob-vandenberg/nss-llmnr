# nss-llmnr: Windows-style name lookup for Linux

## The problem it solves

On a Windows network without its own DNS server, Windows PCs find each other
by short name (like `development`) using a method called **LLMNR**: the PC
asks the network "who is development?" and the owner answers.

A PC with several network cards answers **once per network card**, each time
with a different address. For example, `development` answers with:

1. `10.0.0.141` (the local network)
2. `92.66.200.107` (public)
3. `92.66.200.108` (public)

**Windows** waits for all answers and picks the address on its own network.

**Linux** (systemd-resolved) takes the first answer that arrives and stops.
Which card answers first is luck, so you get a random address, often a public
one.

`nss-llmnr` makes Linux do it the Windows way.

## How it works

When a program looks up a name, Linux goes through a list of places to look,
one after another. That list is the `hosts:` line in `/etc/nsswitch.conf`.
This tool adds one extra place to look, called `llmnr`, right after the
hosts file.

When you look up `development`:

1. **Only short names.** The tool only handles names without dots, like
   `development` or `fileserver`. For a name like `www.google.com` it
   immediately says "not mine", and Linux moves on to the next method.
2. **It asks the network.** It sends the LLMNR question "who is
   development?" on every active network card.
3. **It waits for all answers.** After the first answer arrives, it keeps
   listening for another 250 ms and collects every answer that comes in.
4. **It sorts them like Windows does:**
   1. addresses on your own network first (for example `10.0.0.x`)
   2. then other private addresses (`10.x`, `172.16–31.x`, `192.168.x`)
   3. then public addresses
5. **It hands the sorted list to the program.** Ping, Nemo, the browser and
   so on use the first address in the list.

If nobody answers, it asks up to three times (300 ms each), then gives up,
and Linux tries the next method.

The tool does not run in the background. It only does something at the moment
a program looks up a short name.

## Files

1. The tool: `/lib/x86_64-linux-gnu/libnss_llmnr.so.2`
2. Switched on by the word `llmnr` on the `hosts:` line of
   `/etc/nsswitch.conf`
3. Backup made before installing: `/etc/nsswitch.conf.before-llmnr`
4. Source code: the `nss-llmnr` folder (`nss_llmnr.c`, `Makefile`)

## Installing

### 1. Build and install the tool

In Nemo, open the `nss-llmnr` folder, right-click an empty spot and choose
**Open in Terminal**. Then:

```
sudo apt install build-essential
make
sudo make install
```

### 2. Make a backup of the lookup settings

```
sudo cp /etc/nsswitch.conf /etc/nsswitch.conf.before-llmnr
```

### 3. Switch it on

**Via the UI (Kate):**

1. Open **Kate**, then **File → Open**, and choose `/etc/nsswitch.conf`.
2. Find the line that starts with `hosts:`.
3. Type ` llmnr` directly after `files`. Leave the rest of the line as it
   is. For example:

   ```
   hosts:          files llmnr mdns4_minimal [NOTFOUND=return] dns
   ```

4. Save with **Ctrl+S**. Kate asks for your password because it's a system
   file.

**Or via the terminal:**

```
sudo sed -i 's/^hosts:\(\s*\)files /hosts:\1files llmnr /' /etc/nsswitch.conf
```

### 4. Test

```
grep ^hosts: /etc/nsswitch.conf
getent ahostsv4 development
ping -c 3 development
```

The first address shown by `getent` and `ping` should be the one on your own
network (for `development`: `10.0.0.141`).

Programs that were already open (Nemo, Dolphin) need to be closed and opened
again.

## Uninstalling

### 1. Switch it off

**Via the UI (Kate):**

1. Open `/etc/nsswitch.conf` in Kate.
2. On the `hosts:` line, remove the word ` llmnr`.
3. Save with **Ctrl+S**.

**Or via the terminal**, by restoring the backup:

```
sudo cp /etc/nsswitch.conf.before-llmnr /etc/nsswitch.conf
```

### 2. Remove the tool

In the terminal, in the `nss-llmnr` folder:

```
sudo make uninstall
```

### 3. Clean up (optional)

```
sudo rm /etc/nsswitch.conf.before-llmnr
```

Then delete the `nss-llmnr` folder in Nemo.

## Good to know

1. **Only IPv4**, and only short names without dots.
2. **Unknown short names** take about one extra second before Linux gives
   up, because the tool asks three times.
3. **`resolvectl query development`** does not use this tool; it asks
   systemd-resolved directly. Normal programs (ping, Nemo, browsers) do use it.
4. **Leave LLMNR in systemd-resolved switched off.** It isn't needed with
   this tool, and when it was on, it made reverse lookups slow (a ping took
   about 11 seconds for 3 replies).
