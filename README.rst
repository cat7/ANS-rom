==================================================
Apple Network Server 700/200 for qemu-system-ppc
==================================================

This ``ANS-rom`` branch is a personal fork of QEMU. It adds the Apple
Network Server 700/200 (``-M ans700``) to ``qemu-system-ppc``. The machine
runs Apple's production ROM (Open Firmware 1.1.22) and AIX 4.1.5 for the
Network Server: the install CD, the installed system, the graphics console
and CDE. With the Network Server ROM 2.0 and an ATI Rage 128 Pro it also
starts Mac OS 8.5 from CD, and with the ROM 2.26NT it installs and runs
Windows NT 4.0 for PowerPC. It is an experiment and is not meant for
upstream QEMU.

QEMU as a whole is released under the GNU General Public License, version
2. For full licensing details, consult the LICENSE file.

The machine
===========

=====================  ======================================================
CPU                    PowerPC 604e, 200 MHz, 50 MHz bus, 12.5 MHz time base
Memory controller      Hammerhead; 64 MB default, 512 MB maximum (``-m``)
PCI                    two Bandit host bridges
I/O controller         Grand Central: interrupt controller, DBDMA, ESCC
                       serial ports A and B, Cuda (ADB, clock, power),
                       SWIM3 floppy, AWACS sound, MACE Ethernet, 53C94 SCSI
Fast/wide SCSI         two Symbios 53C825A
Graphics               Cirrus Logic 54M30 (1 MB); ATI Rage 128 Pro in a PCI
                       slot (card ROM needed)
Front panel            4 x 20 character LCD, keyswitch, NVRAM
=====================  ======================================================

The SCSI buses are named:

==========  ==========================  ======================================
Bus         Controller                  Notes
==========  ==========================  ======================================
``scsi.0``  53C94 (Grand Central)       external, 8-bit
``scsi.1``  first 53C825A               internal bays; Open Firmware's
                                        ``disk2`` is ``scsi.1`` id 2
``scsi.2``  second 53C825A
==========  ==========================  ======================================

What you need
=============

- The Network Server ROM, 4 MB, version 1.1.22 (1995-08-21, checksum
  962F6C13). It is not included.
- The AIX 4.1.5 for Apple Network Server install CD as an ISO image. It is
  not included.
- A raw disk image of at least 2 GB for AIX::

    qemu-img create -f raw aix.img 2G

For Mac OS:

- The Network Server ROM 2.0 (1998-01-14, checksum 49B2BE8F). It is not
  included.
- The Mac ROM of an ATI Rage 128 Pro, for example
  ``ati_rage128pro_136_agp.rom``. It is not included.
- A Mac OS 8.5 CD image with its Apple partition map. An image of the bare
  HFS volume does not start. It is not included.

Building
========

.. code-block:: shell

  mkdir build
  cd build
  ../configure --target-list=ppc-softmmu --enable-cocoa --enable-slirp
  make qemu-system-ppc

Only ``qemu-system-ppc`` is needed. ``--enable-cocoa`` is for macOS;
leave it out elsewhere.

Machine options
===============

``-M ans700,option=value,...``

- ``keyswitch=normal|locked|service`` -- the front keyswitch (default
  normal). It can also be turned while the machine runs, from QMP:
  ``qom-set /machine keyswitch service``. AIX reads it at boot
  (``bootinfo -k``).
- ``nvram=FILE`` -- keeps the 8 KB NVRAM (Open Firmware settings) in
  ``FILE``, created if missing. Without it the NVRAM starts blank and is
  not saved.
- ``lcd=CHARDEV`` -- sends the front-panel LCD text to a chardev, one line
  per change, as ``lcdN: <20 characters>``. POST, Open Firmware and the
  AIX boot progress codes all show there.
- ``power-supplies=1|2`` -- how many power supplies the board reports.
- ``pll-cfg=N`` -- the 604e PLL strap (default $A).
- ``onboard-video=on|off`` -- the built-in 54M30 (default on). Off leaves
  an added card as the only screen.
- ``rtc-epoch=unix|mac`` -- the clock counts seconds from 1970 (``unix``,
  default, for AIX) or from 1904 (``mac``, for Mac OS and Windows NT).

Installing AIX
==============

Boot with the key at Service, the CD on ``scsi.1`` id 0 and the empty disk
on ``scsi.1`` id 2. Serial port B is given a telnet socket for the
installer:

.. code-block:: shell

  ./qemu-system-ppc \
    -M ans700,keyswitch=service,nvram=nvram.bin,lcd=lcd0 -m 512 \
    -bios ans700.rom \
    -chardev stdio,id=lcd0 \
    -serial null \
    -chardev socket,id=sb,host=127.0.0.1,port=4556,server=on,wait=off,telnet=on \
    -serial chardev:sb \
    -drive if=none,id=cd,file=AIX_415_Install_CD.iso,media=cdrom,readonly=on \
    -device scsi-cd,drive=cd,bus=scsi.1,scsi-id=0 \
    -drive if=none,id=hd,file=aix.img,format=raw \
    -device scsi-hd,drive=hd,bus=scsi.1,scsi-id=2

Then ``telnet 127.0.0.1 4556``:

1. At "Please define the System Console", type ``2`` and Return on the
   telnet session, which makes serial port B the console.
2. Choose English, then Change/Show Installation Settings: New and
   Complete Overwrite on ``hdisk0``, and start the install.
3. When the installer asks you to turn the key to Normal, press Return.
   AIX reboots; quit QEMU and boot the disk as below.

Running AIX
===========

With the key at Normal and no CD, Open Firmware boots ``disk2:aix``:

.. code-block:: shell

  ./qemu-system-ppc \
    -M ans700,keyswitch=normal,nvram=nvram.bin,lcd=lcd0 -m 512 \
    -bios ans700.rom \
    -chardev stdio,id=lcd0 \
    -serial null \
    -chardev socket,id=sb,host=127.0.0.1,port=4556,server=on,wait=off,telnet=on \
    -serial chardev:sb \
    -drive if=none,id=hd,file=aix.img,format=raw \
    -device scsi-hd,drive=hd,bus=scsi.1,scsi-id=2 \
    -nic user,hostfwd=tcp:127.0.0.1:2323-:23

The boot to ``Console login:`` takes about a minute and a half. Log in as
``root``; a fresh install has no password.

- **Console on the screen.** After the install the console is serial port
  B. ``chcons /dev/lft0`` and a reboot move it to the 54M30 screen and the
  ADB keyboard.
- **Network.** The MACE is ``ent0``. Under ``-nic user``::

    mktcpip -h ans700 -a 10.0.2.15 -m 255.255.255.0 -i en0 -g 10.0.2.2

  The ``hostfwd`` above then lets you ``telnet 127.0.0.1 2323`` into AIX.
- **Clock.** AIX shows the host's time.
- **Shut down** with ``shutdown -F``: Cuda turns the machine off and QEMU
  exits. ``shutdown -Fr`` reboots.

Running Mac OS
==============

Mac OS needs the Network Server ROM 2.0 and a PCI video card with a Mac
driver in its ROM; the 54M30 has none. The Rage 128 Pro goes in slot
``0xd`` to ``0x10`` of the second Bandit, the only slots ROM 2.0 probes
there. Open Firmware runs the card's FCode only when the card's PCI device
id matches the id in its ROM, so give it the ROM's id: ``0x5046`` for
``ati_rage128pro_136_agp.rom``. Put the CD on ``scsi.1`` or ``scsi.0``:

.. code-block:: shell

  ./qemu-system-ppc \
    -M ans700,onboard-video=off,rtc-epoch=mac,nvram=nvram-macos.bin -m 512 \
    -bios ans-rom-2.0.rom \
    -device ati-rage128-pro,bus=bandit@f4000000,addr=0xd,device-id=0x5046,romfile=ati_rage128pro_136_agp.rom \
    -drive if=none,id=cd,file=MacOS85.iso,format=raw,media=cdrom,readonly=on \
    -device scsi-cd,drive=cd,bus=scsi.1,scsi-id=3

It reaches the Finder in about a minute.

- **Screen.** ``onboard-video=off`` leaves the Rage as the only screen. With
  the 54M30 kept, the window opens on the 54M30 (white); cocoa's View menu
  switches to the Rage.
- **CD drive.** Every ``scsi-cd`` on this machine reports itself as an Apple
  CD-ROM drive (MATSHITA CD-ROM CR-8005), which Mac OS's CD driver
  requires.
- **CPU speed.** Mac OS times its delay loops in a short VIA Timer 2 window;
  the calibration governor paces the CPU inside that window, so no
  ``-icount`` is needed.
- Tested: Mac OS 8.5 from CD to the Finder, on ``scsi.1`` and ``scsi.0``.
  ``scsi.2`` and installing Mac OS on a disk are untested.

CDE
===

CDE 4.1.3, the X server and Motif are on the install CD but are not part
of the base install. Attach the CD as for the install (key at Normal) and,
as root::

  mkdir -p /cdrom
  mount -v cdrfs -o ro /dev/cd0 /cdrom
  installp -acgXd /cdrom/usr/sys/inst.images \
    X11.base X11.Dt X11.apps X11.fnt X11.motif X11.loc.en_US X11.msg.en_US

Installing ``X11.Dt`` turns on the graphical login. The 54M30 starts at
640 x 480, where the CDE front panel does not fit. For 1024 x 768::

  chdev -l gda0 -a GenericVGA=1024x768@60Hz -P
  shutdown -Fr

The keyboard and the mouse work on the desktop.

Running Windows NT
==================

Windows NT 4.0 for PowerPC runs on the Network Server ROM 2.26NT, whose
Open Firmware can run little-endian, with the hardware layer (HAL), ADB
driver and ARC firmware of powermac-nt-hal on a boot floppy
(github.com/cat7/nt-boot-floppy, from pappadf/powermac-nt-hal). The
machine models the 604's little-endian mode and Bandit's byte-lane
reversal for it, and the 54M30's BitBLT engine for NT's Cirrus driver.

The disk needs a FAT system partition, which NT's Setup cannot make on this
machine; the GUI makes such a disk ("For: Windows NT"). The CD goes on
``scsi.1`` ID 0, the disk on ``scsi.2`` ID 0:

.. code-block:: shell

  ./qemu-system-ppc \
    -M ans700,rtc-epoch=mac,nvram=nvram-nt.bin -m 64 \
    -bios ans-rom-2.26NT.rom \
    -drive if=none,id=cd,file=winnt40.iso,format=raw,media=cdrom,readonly=on \
    -device scsi-cd,drive=cd,bus=scsi.1,scsi-id=0 \
    -drive if=none,id=hd,file=nt-disk.img,format=raw \
    -device scsi-hd,drive=hd,bus=scsi.2,scsi-id=0 \
    -drive if=none,id=fd0,file=nt-boot-floppy.img,format=raw \
    -global swim3.drive=fd0

At the ``0 >`` prompt, once per NVRAM (Open Firmware turns little-endian
and resets)::

  load fd:,\setup.of
  load-base loadsize eval

Then ``\boot.of`` the same way to install from the CD, and
``\bootdisk.of`` to start the installed system. At Setup's mass-storage
screen add the floppy's ADB/OEM-disk driver (S, Other). An NVRAM set up
for NT no longer starts AIX or Mac OS.

- Tested: NT 4.0 Workstation (build 1381) installed from CD and started
  to the desktop with the Open Firmware console on ttya; with the console
  on the screen, Setup up to its licence screen.

Known limits
============

- One CPU. POST shows 195 MHz for the 200 MHz part (the ROM's own speed
  table).
- Open Firmware's ``probe-scsi0`` on the 53C94 bus with a device attached
  is untested since the bus got its DMA channel. AIX and Mac OS use the bus.
- The 53C94 answers an empty SCSI id at once instead of after the
  selection time-out.
- Mac OS: only 8.5 starting from CD is tested.
- Windows NT: no network driver (MACE); the 54M30 only, no Rage 128.
  About one start in four of the installed system hangs in Open Firmware
  before NT; start it again.
- Open Firmware network boot is untested.
- The CDE and lft consoles were tested in English only.
