This version should compile on 64 bit systems.
Start "jackbridge" and connect its jack ins and outs 
before starting the wineasio application.

Before installation edit the prefix path in the Makefile
PREFIX = <root path you use>

usually this will either be

PREFIX = /usr
or
PREFIX = /usr/local

Copy the file asio.h from Steinberg's asio-sdk to
the wineasio directory

then execute: make
and as root:  make install

then, again as normal user: regsvr32 wineasio.dll

original code: Robert Reif posted to the wine mailinglist
modified by: Ralf Beck (musical_snake@gmx.de)
             and Peter L Jones

todo: 
- make timecode sync to jack transport


changelog:
-X:
rewrite for use with a 64 bit jackd

0.3:
30-APR-2007: corrected connection of in/outputs
