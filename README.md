# ATOPHTTPD

## Introduction of [atop](https://www.atoptool.nl)
<img align="right" width="100" height="116" src="http://www.atoptool.nl/images/atoplogo.png">

Atop is an ASCII full-screen performance monitor for Linux that is capable
of reporting the activity of all processes (even if processes have finished
during the interval), daily logging of system and process activity for
long-term analysis, highlighting overloaded system resources by using colors,
etcetera. At regular intervals, it shows system-level activity related to the
CPU, memory, swap, disks (including LVM) and network layers, and for every
process (and thread) it shows e.g. the CPU utilization, memory growth,
disk utilization, priority, username, state, and exit code.
In combination with the optional kernel module *netatop*,
it even shows network activity per process/thread.
In combination with the optional daemon *atopgpud*,
it also shows GPU activity on system level and process level.

## Introduction of [atophttpd](https://github.com/pizhenwei/atophttpd)
atop records the system level and process level information into log files
as a daemon process, then an end user accesses the server, runs command
`atop -r /var/log/atop/atop_20230105 -b 12:34` to analyze the performance,
system status, process status at a specified time stamp.

atophttpd runs as a daemon, reads the atop log files, and provide a HTTP 1.1
service. This allows to use atop by a web browser without server login, it's
also possible to query the system level and process level information in
batch.

## HOWTO
```
 make
 ./atophttpd -d #run in daemon
```

then access atophttpd by web browser, for example: `192.168.1.100:2867`
