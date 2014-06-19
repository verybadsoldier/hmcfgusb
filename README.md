hmcfgusb
========

github mirror of http://git.zerfleddert.de/cgi-bin/gitweb.cgi/hmcfgusb

I am not the author of this package, i'm just mirroring it here on github, all credits go to 
Michael Gernoth http://git.zerfleddert.de/cgi-bin/gitweb.cgi/hmcfgusb

--------------------------------------------------------------------------------------

    This repository contains utilities to use the HM-CFG-USB(2) (HomeMatic USB Konfigurations-Adapter) from ELV on    Linux/Unix by using libusb 1.0.
    The HM-CFG-USB can be used to send and receive BidCoS-Packets to control HomeMatic home automation devices (like remote controllable sockets, switches, sensors, ...).

    This repository contains, amongst others, an application, which emulates the HomeMatic LAN configuration adapter-protocol to make it possible to use the HM-CFG-USB in Fhem or as a lan configuration tool for the CCU or the HomeMatic windows configuration software.

    Short hmland HowTo:
    Install prerequisites: apt-get install libusb-1.0-0-dev make gcc
    Get the current version of this software: hmcfgusb-HEAD-xxxxxxx.tar.gz (xxxxxxx is part of the commit-id. xxxxxxx is just a placeholder for this HowTo, use your value)
    Extract the archive: tar xzf hmcfgusb-HEAD-xxxxxxx.tar.gz
    Change into the new directory: cd hmcfgusb-HEAD-xxxxxxx
    Build the code: make
    Optional: Install udev-rules so normal users can access the device: sudo cp hmcfgusb.rules /etc/udev/rules.d/
    Plug in the HM-CFG-USB
    Run hmland (with debugging the first time, see -h switch): ./hmland -p 1234 -D
    Configure Fhem to use your new HMLAN device:
    define hmusb HMLAN 127.0.0.1:1234
    attr hmusb hmId <hmId>
    Updating the HM-CFG-USB firmware to version 0.967:
    Compile the hmcfgusb utilities like in the hmland HowTo above (steps 1 to 7) and stay in the directory
    Download the new firmware: hmusbif.03c7.enc: wget https://git.zerfleddert.de/hmcfgusb/firmware/hmusbif.03c7.enc
    Make sure that hmland is not running
    Flash the update to the USB-stick: ./flash-hmcfgusb hmusbif.03c7.enc (You might need to use sudo for this)
    (Old) Prebuilt package for OpenWRT (ar71xx): hmcfgusb_1_ar71xx.ipk
