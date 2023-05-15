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
### run atop daemon
* By systemd: `systemctl status atop.service`(test atop service) and `systemctl start atop.service`(start atop service).
* By command: `atop -w /var/log/atop/atop_20230105 10`.

### run atophttpd daemon:
```
 make
 ./atophttpd -d #run in daemon only on localhost
 ./atophttpd -d -a ${IP} #run in daemon on ip
```

### access atophttpd server:
* By a web browser, for example: `192.168.1.100:2867`,
to get the help page by `192.168.1.100:2867/help`.

* By curl command: `curl 'http://127.0.0.1:2867/showsamp?lables=ALL&timestamp=1675158274&encoding=none' | jq `.

### Generate TLS certification:
```
 bash gen-cert.sh
```
   * CertFile will be generated under `tls/`

### run atophttpd daemon with TLS:
```
 make USE_TLS=YES
 /atophttpd -t 2868 -C tls/ca.crt -c tls/server.crt -k tls/server.key
```

Or use default TLS config:

* CA cert file default use `/etc/pki/CA/ca.crt`
* Server cert file default use `/etc/pki/atophttpd/server.crt`
* Server key file default use `/etc/pki/atophttpd/server.key`

### access atophttpd server with TLS:
* By curl command:
```
curl --cacert tls/ca.crt --cert tls/client.crt --key tls/client.key 'https://127.0.0.1:2868/showsamp?lables=ALL&timestamp=1684402523&encoding=none'
```

