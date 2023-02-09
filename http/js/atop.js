/*
 ** This is a JavaScript for ATOP Http Server
 ** 
 ** ==========================================================================
 ** Author:      Enhua Zhou
 ** E-mail:      zhouenhua@bytedance.com
 ** Date:        September 2022
 ** --------------------------------------------------------------------------
 ** Copyright (C) 2022 Enhua Zhou <zhouenhua@bytedance.com>
 **
 ** This program is free software; you can redistribute it and/or modify it
 ** under the terms of the GNU General Public License as published by the
 ** Free Software Foundation; either version 2, or (at your option) any
 ** later version.
 **
 ** This program is distributed in the hope that it will be useful, but
 ** WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 ** See the GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program; if not, write to the Free Software
 ** Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 ** --------------------------------------------------------------------------
 */

const OrderType = {
    CPU: 'CPU',
    DISK: 'DISK',
    MEM: 'MEM',
}
Object.freeze(OrderType);

const TempleteType = {
    CommandLine: 'command_line',
    Disk: 'disk',
    Generic: 'generic',
    Memory: 'memory'
}
Object.freeze(TempleteType);

const DEFAULT_PROC_SHOW_NUM = 200;

const DEFAULT_CPU_SHOW_NUM = 0;
const DEFAULT_GPU_SHOW_NUM = 2;
const DEFAULT_DISK_SHOW_NUM = 1;
const DEFAULT_LVM_SHOW_NUM = 4;
const DEFAULT_INTERFACE_SHOW_NUM = 2;
const DEFAULT_INFINIBAND_SHOW_NUM = 2;
const DEFAULT_NFS_SHOW_NUM = 2;
const DEFAULT_CONTAINER_SHOW_NUM = 1;
const DEFAULT_NUMA_SHOW_NUM = 0;
const DEFAULT_LLC_SHOW_NUM = 0;

var proc_show_num = DEFAULT_PROC_SHOW_NUM;
var cpu_show_num = DEFAULT_CPU_SHOW_NUM;
var gpu_show_num = DEFAULT_GPU_SHOW_NUM;
var lvm_show_num = DEFAULT_LVM_SHOW_NUM;
var disk_show_num = DEFAULT_DISK_SHOW_NUM;
var interface_show_num = DEFAULT_INTERFACE_SHOW_NUM;
var infiniband_show_num = DEFAULT_INFINIBAND_SHOW_NUM;
var nfs_show_num = DEFAULT_NFS_SHOW_NUM;
var container_show_num = DEFAULT_CONTAINER_SHOW_NUM;
var numa_show_num = DEFAULT_NUMA_SHOW_NUM;
var LLC_show_num = DEFAULT_LLC_SHOW_NUM;

// second
var pre_timestamp = Date.parse(new Date()) / 1000;

var template_type = TempleteType.Generic;
var order_type = OrderType.CPU;
var only_proc = true;

var change_template = false
var change_data = false
var init = true

var delta = 0
const queue = []
let loading = false
let p

var cache_template_header = ""
var cache_template_process = ""
var cache_json_data = ""

window.onkeydown = function (event) {
    var keyCode = event.key;
    now = Date.parse(new Date()) / 1000;

    switch (keyCode) {
        case 'b':
            var now_date_str = new Date(pre_timestamp * 1000).format("YYYYMMDDhhmm");

            var chosed_date_str = prompt("Enter new time (format [YYYYMMDD]hhmm): ", now_date_str);
            if (chosed_date_str != null) {
                var tmp_timestamp = 0;
                tmp_timestamp = ParseDateToTimesample(chosed_date_str)
                if (tmp_timestamp != 0) {
                    delta = tmp_timestamp - pre_timestamp
                } else {
                    alert("Wrong time!")
                }
            }
            change_data = true
            break;
        case 'T':
            delta -= 10
            if (loading) {
                return
            }
            change_data = true
            break;
        case 't':
            delta += 10
            if (loading) {
                return
            }
            change_data = true
            break;
        case 'l':
            atopList();
            change_template = true
            break;
        case 'g':
            order_type = OrderType.CPU;
            change_template = true;
            template_type = TempleteType.Generic;
            break;
        case 'm':
            order_type = OrderType.MEM;
            change_template = true;
            template_type = TempleteType.Memory;
            break;
        case 'd':
            order_type = OrderType.DISK;
            change_template = true;
            template_type = TempleteType.Disk;
            break;
        case 'c':
            order_type = OrderType.DISK;
            change_template = true;
            template_type = TempleteType.CommandLine;
            break;
        case 'y':
            only_proc = !only_proc;
            change_template = true;
            break;
        default:
            return
    }

    main();
}

window.onload = function () {
    change_data = true
    change_template = true
    main();
}

function atopList() {
    var new_process_show_num = prompt("Maxinum lines for process statistics (now " + proc_show_num + "): ", DEFAULT_PROC_SHOW_NUM);
    if (new_process_show_num != null) {
        proc_show_num = new_process_show_num;
    }

    var new_cpu_show_num = prompt("Maxinum lines for per-cpu statistics (now " + cpu_show_num + "): ", DEFAULT_CPU_SHOW_NUM);
    if (new_cpu_show_num != null) {
        cpu_show_num = new_cpu_show_num;
    }

    var new_gpu_show_num = prompt("Maxinum lines for per-gpu statistics (now " + gpu_show_num + "): ", DEFAULT_GPU_SHOW_NUM);
    if (new_gpu_show_num != null) {
        gpu_show_num = new_gpu_show_num;
    }

    var new_lvm_show_num = prompt("Maxinum lines for LVM statistics (now " + lvm_show_num + "): ", DEFAULT_LVM_SHOW_NUM);
    if (new_lvm_show_num != null) {
        lvm_show_num = new_lvm_show_num;
    }

    var new_disk_show_num = prompt("Maxinum lines for disk statistics (now " + disk_show_num + "): ", DEFAULT_DISK_SHOW_NUM);
    if (new_disk_show_num != null) {
        disk_show_num = new_disk_show_num;
    }

    var new_interface_show_num = prompt("Maxinum lines for interface statistics (now " + interface_show_num + "): ", DEFAULT_INTERFACE_SHOW_NUM);
    if (new_interface_show_num != null) {
        interface_show_num = new_interface_show_num;
    }

    var new_infiniband_show_num = prompt("Maxinum lines for infiniband port statistics (now " + infiniband_show_num + "): ", DEFAULT_INFINIBAND_SHOW_NUM);
    if (new_infiniband_show_num != null) {
        infiniband_show_num = new_infiniband_show_num;
    }

    var new_nfs_show_num = prompt("Maxinum lines for NFS mount statistics (now " + nfs_show_num + "): ", DEFAULT_NFS_SHOW_NUM);
    if (new_nfs_show_num != null) {
        nfs_show_num = new_nfs_show_num;
    }

    var new_container_show_num = prompt("Maxinum lines for container statistics (now " + container_show_num + "): ", DEFAULT_CONTAINER_SHOW_NUM);
    if (new_container_show_num != null) {
        container_show_num = new_container_show_num;
    }

    var new_numa_show_num = prompt("Maxinum lines for numa statistics (now " + numa_show_num + "): ", DEFAULT_NUMA_SHOW_NUM);
    if (new_numa_show_num != null) {
        numa_show_num = new_numa_show_num;
    }

    var new_LLC_show_num = prompt("Maxinum lines for LLC statistics (now " + LLC_show_num + "): ", DEFAULT_LLC_SHOW_NUM);
    if (new_LLC_show_num != null) {
        LLC_show_num = new_LLC_show_num;
    }
}

function main() {
    var req_timestamp = pre_timestamp + delta
    var req_template_type = template_type
    var req_change_data = change_data
    var req_change_template = change_template
    if (delta == 0 && init == true) {
        getHtmlTemplateProcessFromServer(req_timestamp, req_template_type, req_change_data, req_change_template).then(getHtmlTemplateHeaderFromServer).then(getJsonRawDataFromServer).then(res => {
            LoadHtmlFromJson(cache_json_data, constructTemplate(cache_template_header, cache_template_process))
            while (p = queue.shift()) {
                p.resolve()
            }
            init = false
            loading = false
            change_data = false
            change_template = false
        })
        return
    } else {
        delta = 0
        getHtmlTemplateProcessFromServer(req_timestamp, req_template_type, req_change_data, req_change_template).then(getJsonRawDataFromServer).then(res => {
            LoadHtmlFromJson(cache_json_data, constructTemplate(cache_template_header, cache_template_process))
            while (p = queue.shift()) {
                p.resolve()
            }
            loading = false
            change_data = false
            change_template = false
        })
    }
}

function constructTemplate(template_header, template_process) {
    return '<!DOCTYPE html><html><body><template id="tpl">' + template_header + template_process + ' </template></body></html>'
}

function getHtmlTemplateProcessFromServer(req_timestamp, req_template_type, req_change_data, req_change_template) {
    return new Promise((resolve, reject) => {
        if (loading) {
            queue.push({ resolve, reject })
        }
        if (!loading) {
            loading = true

            if (req_change_template) {
                host = document.location.host;
                var url = "http://" + host + "/template?type=" + req_template_type;
                var request = new XMLHttpRequest();

                request.open("GET", url, true);
                request.send();
                request.onload = function () {
                    if (request.status === 200) {
                        raw_template_process = request.responseText;
                        cache_template_process = raw_template_process
                        resolve({
                            req_timestamp,
                            req_change_data
                        })
                    }
                }
            } else {
                resolve({ req_timestamp, req_change_data })
            }
        }
    })
}

function getHtmlTemplateHeaderFromServer(paras) {
    return new Promise((resolve, reject) => {
        if (init) {
            host = document.location.host;
            var url = "http://" + host + "/template_header"
            var request = new XMLHttpRequest();

            var req_timestamp = paras.req_timestamp
            var req_change_data = paras.req_change_data

            request.open("GET", url, true);
            request.send();
            request.onload = function () {
                if (request.status === 200) {
                    raw_template_header = request.responseText;
                    cache_template_header = raw_template_header
                    resolve({
                        req_timestamp,
                        req_change_data
                    })
                }
            }
        } else {
            resolve({ req_timestamp, req_change_data })
        }
    })
}

function getJsonRawDataFromServer(paras) {
    return new Promise((resolve, reject) => {
        if (paras.req_change_data) {
            host = document.location.host;
            var url = "http://" + host + "/showsamp?timestamp=" + paras.req_timestamp + "&lables=ALL";
            var request = new XMLHttpRequest();

            request.open("GET", url, true);
            request.send();
            request.onload = function () {
                if (request.status === 200) {
                    raw_json = request.responseText;
                    cache_json_data = raw_json
                    resolve()
                }
            }
        } else {
            resolve()
        }
    })
}

function LoadHtmlFromJson(raw_json, template) {
    var parser = new DOMParser();
    doc = parser.parseFromString(template, "text/html");
    let tpl = doc.getElementById('tpl').innerHTML;
    let node = HtmlToNode(tpl);
    document.getElementById('target').innerHTML = ParseJsonToHtml(node, raw_json);
}

function ParseJsonToHtml(node, raw_json) {
    var nest_json = "[" + raw_json + "]";
    var json = JSON.parse(nest_json);
    preprocessJson(json);

    // sec to us
    per_cpu_tot = json[0]["EXTRA"]["percputot"];
    hertz = json[0]["CPU"]["hertz"];
    return repeatAtopHtmlNode(node, json, per_cpu_tot, hertz);
}

function preprocessJson(json) {
    parseAtopHeader(json);
    parseAtopProcess(json);
}

function repeatAtopHtmlNode(node, arr, percputot) {
    let out = [];

    for (let i = 0; i < arr.length; i++) {
        let tmp = node.outerHTML;
        tmp = tmp.replace(/\s/g, ' ');

        let map = arr[i];
        // parse inner array
        for (let j in map) {
            if (map[j] instanceof Array) {
                let subNode = node.querySelector('.' + j);
                if (subNode) {
                    let subHTML = repeatAtopHtmlNode(subNode, map[j], percputot, hertz);
                    let subTpl = subNode.outerHTML.replace(/\s/g, ' ');
                    tmp = tmp.replace(subTpl, subHTML);
                }
            }
        }

        // parse Object
        for (let j in map) {
            if (map[j] instanceof Object && !(map[j] instanceof Array)) {
                let subNode = node.querySelector('.' + j);
                if (subNode) {
                    let subHTML = repeatAtopHtmlNode(subNode, [map[j]], percputot, hertz);
                    let subTpl = subNode.outerHTML.replace(/\s/g, ' ');
                    tmp = tmp.replace(subTpl, subHTML);
                }
            }
        }

        for (let j in map) {
            if (typeof map[j] === 'string' || typeof map[j] === 'number') {
                if (["stime", "utime", "ntime", "itime", "wtime", "Itime", "Stime", "steal", "guest"].indexOf(j) !== -1) {
                    map[j] = ParseCPUPercentValue(map[j], percputot);
                } else if (["cpusome", "memsome", "memfull", "iosome", "iofull"].indexOf(j) !== -1) {
                    map[j] = ParsePSIPercentValue(map[j]);
                } else if (["freq"].indexOf(j) !== -1) {
                    map[j] = ParseCPUCurfValue(map[j]);
                } else if (["stime_unit_time", "utime_unit_time", "rundelay", "blkdelay"].indexOf(j) !== -1) {
                    map[j] = ParseTimeValue(map[j], hertz, j);
                } else if (["buffermem", "cachedrt", "cachemem", "commitlim", "committed", "freemem", "freeswap",
                    "filepage", "physmem", "rgrow", "rsz", "shmem", "shmrss", "shmswp", "slabmem", "swcac", "totmem", "totswap", "slabreclaim", "pagetables",
                    "vexec", "vdata", "vgrow", "vlibs", "vlock", "vmem", "vstack", "wsz", "rmem", "pmem", "vswap", "tcpsk", "udpsk", "dirtymem", "active",
                    "mbm_total", "mbm_local", "inactive"].indexOf(j) !== -1) {
                    map[j] = ParseMemValue(map[j], j);
                } else if (["rbyte", "sbyte", "speed"].indexOf(j) !== -1) {
                    map[j] = ParseBandwidth(map[j], j);
                } else if (["minflt", "majflt"].indexOf(j) !== -1) {
                    map[j] = Value2ValueStr(map[j], j);
                } else if (["cpubusy", "membusy", "diskbusy", "lvmbusy", "frag", "occupancy"].indexOf(j) !== -1) {
                    map[j] = Value2ValuePercent(map[j], j);
                }
                else {
                    map[j] = map[j];
                }

                if (typeof map[j] === 'string') {
                    map[j] = map[j];
                }

                let re = new RegExp('{' + j + '}', 'g');
                tmp = tmp.replace(re, map[j]);
            }
        }

        out.push(tmp);
    }
    return out.join('');
}