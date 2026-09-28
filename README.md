# nss-llmnr

Makes Linux resolve Windows computer names (LLMNR) the way Windows does:
ask on all network cards, **wait for all answers**, then use the address in
your own subnet first. (See RFC 4795)

Without this, Linux (systemd-resolved) takes the first answer that arrives.
For a Windows PC with several network cards that is a random address.

## Build and install

    sudo apt install build-essential
    tar xzf nss-llmnr.tar.gz
    cd nss-llmnr
    make
    sudo make install

## Activate

Back up the lookup settings, then put `llmnr` right after `files`:

    sudo cp /etc/nsswitch.conf /etc/nsswitch.conf.before-llmnr
    sudo sed -i 's/^hosts:\(\s*\)files /hosts:\1files llmnr /' /etc/nsswitch.conf
    grep ^hosts: /etc/nsswitch.conf

The line should start with `hosts: files llmnr ...`.
Takes effect immediately for newly started programs (restart Nemo/Dolphin).

## Test

    getent ahostsv4 development
    ping -c 2 development

## Undo

    sudo cp /etc/nsswitch.conf.before-llmnr /etc/nsswitch.conf
    sudo make uninstall

## Behaviour

* Only single-label names (`development`, not `www.example.com`).
* Sends the LLMNR query on every active IPv4 network card.
* After the first answer, keeps collecting for 250 ms, then sorts:
  own subnet first, then private (10.x, 172.16-31.x, 192.168.x), then the rest.
* Unknown names: 3 attempts of 300 ms, then the next method in
  nsswitch.conf takes over.
* IPv4 only.
