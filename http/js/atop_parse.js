function parseAtopHeader(json) {
    // Modified the global
    pre_timestamp = json[0]["timestamp"];

    // Parse date
    var now = new Date(json[0]["timestamp"] * 1000);
    y = now.getFullYear();
    m = now.getMonth() + 1;
    d = now.getDate();
    format_date = y + "-" + (m < 10 ? "0" + m : m) + "-" + (d < 10 ? "0" + d : d) + " " + now.toTimeString().slice(0, 8);
    json[0]["date"] = format_date;

    // Parse elapsed
    elapsed = json[0]["elapsed"];

    //CPU line
    CPUEntry = json[0]["CPU"];
    CPU_Tot = CPUEntry["stime"] + CPUEntry["utime"] + CPUEntry["ntime"] + CPUEntry["itime"] + CPUEntry["wtime"] + CPUEntry["Itime"] + CPUEntry["Stime"] + CPUEntry["steal"];

    if (CPU_Tot === 0) {
        CPU_Tot = 1;
    }

    per_cpu_tot = CPU_Tot / CPUEntry["nrcpu"];
    mstot = CPU_Tot * 1000 / CPUEntry["hertz"] / CPUEntry["nrcpu"];

    json[0]["CPU"]["cpubusy"] = ((CPU_Tot - CPUEntry["itime"] - CPUEntry["wtime"]) / per_cpu_tot * 100).toFixed(1);

    //cpu line
    cpulist = json[0]["cpu"];
    cpulist.sort(compareOrderByCpu);

    if (cpu_show_num > cpulist.length) {
        cpu_show_num = cpulist.length;
    }
    json[0]["cpu"] = cpulist.slice(0, cpu_show_num);

    //GPU line
    //TODO

    //CPL line
    json[0]["CPL"]["nrcpu"] = CPUEntry["nrcpu"];

    //MEM line

    //SWP line

    //NUM NUC line
    json[0]["MEM"]["numanode"] = json[0]["NUM"].length;

    numa_memlist = json[0]["NUM"];
    numa_memlist.forEach(function (numanode, index) {
        numanode["numanodeId"] = index;
        numanode["frag"] = (numanode["frag"] * 1).toFixed(2)
    })

    numa_cpulist = json[0]["NUC"];
    if (numa_show_num > numa_cpulist.length) {
        numa_show_num = numa_cpulist.length;
    }

    json[0]["NUM"] = numa_memlist.slice(0, numa_show_num);
    json[0]["NUC"] = numa_cpulist.slice(0, numa_show_num);

    //PAG line

    //PSI line
    psiEntry = json[0]["PSI"];
    json[0]["PSI"]["cpusome"] = ((psiEntry["cstot"] / elapsed * 100) > 100) ? 100 : psiEntry["cstot"] / (elapsed * 10000);
    json[0]["PSI"]["iosome"] = ((psiEntry["iostot"] / elapsed * 100) > 100) ? 100 : psiEntry["iostot"] / (elapsed * 10000);
    json[0]["PSI"]["iofull"] = ((psiEntry["ioftot"] / elapsed * 100) > 100) ? 100 : psiEntry["ioftot"] / (elapsed * 10000);
    json[0]["PSI"]["memsome"] = ((psiEntry["mstot"] / elapsed * 100) > 100) ? 100 : psiEntry["mstot"] / (elapsed * 10000);
    json[0]["PSI"]["memfull"] = ((psiEntry["mftot"] / elapsed * 100) > 100) ? 100 : psiEntry["mftot"] / (elapsed * 10000);
    json[0]["PSI"]["cs"] = psiEntry["cs10"].toFixed(0) + "/" + psiEntry["cs60"].toFixed(0) + "/" + psiEntry["cs300"].toFixed(0);
    json[0]["PSI"]["ms"] = psiEntry["ms10"].toFixed(0) + "/" + psiEntry["ms60"].toFixed(0) + "/" + psiEntry["ms300"].toFixed(0);
    json[0]["PSI"]["mf"] = psiEntry["mf10"].toFixed(0) + "/" + psiEntry["mf60"].toFixed(0) + "/" + psiEntry["mf300"].toFixed(0);
    json[0]["PSI"]["is"] = psiEntry["ios10"].toFixed(0) + "/" + psiEntry["ios60"].toFixed(0) + "/" + psiEntry["ios300"].toFixed(0);
    json[0]["PSI"]["if"] = psiEntry["iof10"].toFixed(0) + "/" + psiEntry["iof60"].toFixed(0) + "/" + psiEntry["iof300"].toFixed(0);

    //LVM line
    lvmlist = json[0]["LVM"];
    lvmlist.forEach(function (lvm, index) {
        lvm["lvmname"] = lvm["lvmname"].substring(lvm["lvmname"].length - 10)
        iotot = lvm["nread"] + lvm["nwrite"];
        lvm["avio"] = (iotot ? lvm["io_ms"] / iotot : 0.0).toFixed(2);
        lvm["lvmbusy"] = (mstot ? lvm["io_ms"] / mstot : 0).toFixed(2);
        lvm["MBr/s"] = (lvm["nrsect"] / 2 / 1024 / elapsed).toFixed(1);
        lvm["MBw/s"] = (lvm["nwsect"] / 2 / 1024 / elapsed).toFixed(1);
    })

    //DSK line
    dsklist = json[0]["DSK"];
    dsklist.forEach(function (dsk, index) {
        iotot = dsk["nread"] + dsk["nwrite"];
        dsk["avio"] = (iotot ? dsk["io_ms"] / iotot : 0.0).toFixed(2);
        dsk["diskbusy"] = (mstot ? dsk["io_ms"] / mstot : 0).toFixed(2);
        dsk["MBr/s"] = (dsk["nrsect"] / 2 / 1024 / elapsed).toFixed(1);
        dsk["MBw/s"] = (dsk["nwsect"] / 2 / 1024 / elapsed).toFixed(1);
    })
    dsklist.sort(compareOrderByDiskIoms);
    if (disk_show_num > dsklist.length) {
        disk_show_num = dsklist.length;
    }
    json[0]["DSK"] = dsklist.slice(0, disk_show_num);

    //NET line
    netlist = json[0]["NET"];
    netlist.sort(compareOrderByNet);
    if (interface_show_num > netlist.length) {
        interface_show_num = netlist.length
    }
    json[0]["NET"] = netlist.slice(0, interface_show_num);

    //IFB line
    // TODO

    // LLC line
    llclist = json[0]["LLC"];
    llclist.sort(compareOrderLLC);
    json[0]["LLC"] = llclist.slice(0, LLC_show_num)

    // EXTRA
    extra = new Object();
    extra["mstot"] = mstot;
    extra["percputot"] = per_cpu_tot;

    extra["availmem"] = json[0]["MEM"]["physmem"] / 1024;
    json[0]["EXTRA"] = extra;
}

function parseAtopProcess(json) {
    percputot = json[0]["EXTRA"]["percputot"];
    availmem = json[0]["EXTRA"]["availmem"];

    prc = json[0]["PRC"];
    prm = json[0]["PRM"];
    prd = json[0]["PRD"];
    prg = json[0]["PRG"];

    prcmap = ArrayToMap(prc);
    prmmap = ArrayToMap(prm);
    prdmap = ArrayToMap(prd);
    prgmap = ArrayToMap(prg);

    hprc = new Object();
    hprc["proc_count"] = 0;
    hprc["sleep_count"] = 0;
    hprc["zombie_count"] = 0;
    hprc["exit_count"] = 0;
    hprc["running_count"] = 0;
    hprc["sleep_interrupt_count"] = 0;
    hprc["sleep_uninterrupt_count"] = 0;

    hprc["stime_unit_time"] = 0;
    hprc["utime_unit_time"] = 0;

    prgmap.forEach(function (iprg, index) {
        if (iprg["state"] == "E") {
            hprc["exit_count"]++;
        } else {
            if (iprg["state"] == "Z") {
                hprc["zombie_count"]++;
            }
            hprc["sleep_interrupt_count"] += iprg["nthrslpi"];
            hprc["sleep_uninterrupt_count"] += iprg["nthrslpu"];
            hprc["running_count"] += iprg["nthrrun"];
            hprc["proc_count"]++;
        }
    })

    // get availdsk
    var availdisk = 0;
    prdmap.forEach(function (iprd, index) {
        var nett_wsz = 0;
        if (iprd["wsz"] > iprd["cwsz"]) {
            nett_wsz = iprd["wsz"] - iprd["cwsz"];
        } else {
            nett_wsz = 0;
        }

        availdisk += iprd["rsz"];
        availdisk += nett_wsz;
    })

    aggregate_pr_map = new Map();
    prcmap.forEach(function (iprc, index) {
        // proc_count not contains exit proc

        iprm = prmmap.get(index);
        iprd = prdmap.get(index);
        iprg = prgmap.get(index);

        if (iprg["isproc"] !== 1) {
            iprg["tid"] = iprg["tgid"];
            if (only_proc) {
                return;
            }
        } else {
            iprg["tid"] = '-';
        }

        hprc["stime_unit_time"] += iprc["stime"];
        hprc["utime_unit_time"] += iprc["utime"];

        //cpu
        cputot = iprc["stime"] + iprc["utime"];
        iprc["cpubusy"] = ((cputot / percputot) * 100).toFixed(1);

        iprc["stime_unit_time"] = iprc["stime"];
        iprc["utime_unit_time"] = iprc["utime"];
        iprc["curcpu"] = iprc["curcpu"] ? iprc["curcpu"] : "-";

        //general
        pro_name = iprg["name"].replace(/[()]/ig, "");
        iprg["name"] = pro_name.length > 15 ? pro_name.substr(0, 15) : pro_name;
        cmdline = iprg["cmdline"].replace(/[()]/ig, "");
        if (cmdline.length == 0) {
            iprg["cmdline"] = iprg["name"]
        } else {
            iprg["cmdline"] = cmdline
        }
        iprg["exitcode"] = iprg["exitcode"] ? iprg["exitcode"] : "-";

        //mem
        membusy = (iprm["rmem"] * 100 / availmem).toFixed(1)
        if (membusy > 100) {
            iprm["membusy"] = 100;
        } else {
            iprm["membusy"] = membusy;
        }

        iprm["vexec"] = iprm["vexec"] * 1024;
        iprm["vlibs"] = iprm["vlibs"] * 1024;
        iprm["vdata"] = iprm["vdata"] * 1024;
        iprm["vstack"] = iprm["vstack"] * 1024;
        iprm["vlock"] = iprm["vlock"] * 1024;
        iprm["vmem"] = iprm["vmem"] * 1024;
        iprm["rmem"] = iprm["rmem"] * 1024;
        iprm["pmem"] = iprm["pmem"] * 1024;
        iprm["vgrow"] = iprm["vgrow"] * 1024;
        iprm["rgrow"] = iprm["rgrow"] * 1024;
        iprm["vswap"] = iprm["vswap"] * 1024;

        //disk
        var nett_wsz = 0;
        if (iprd["wsz"] > iprd["cwsz"]) {
            nett_wsz = iprd["wsz"] - iprd["cwsz"];
        } else {
            nett_wsz = 0;
        }

        diskbusy = ((iprd["rsz"] + nett_wsz) * 100 / availdisk).toFixed(1)
        if (diskbusy > 100) {
            iprd["diskbusy"] = 100;
        } else {
            iprd["diskbusy"] = diskbusy;
        }

        iprd["rsz"] = iprd["rsz"] * 512;
        iprd["wsz"] = iprd["wsz"] * 512;
        iprd["cwsz"] = iprd["cwsz"] * 512;

        aggregate_pr = Object.assign(iprc, iprm, iprd, iprg);

        aggregate_pr_map.set(index, aggregate_pr);
    })

    // map to arr for sort
    aggregate_pr_arr = [];
    aggregate_pr_map.forEach(function (value, key) {
        aggregate_pr_arr.push(value);
    })

    switch (order_type) {
        case OrderType.CPU:
            aggregate_pr_arr.sort(compareOrderByCpu);
            break;
        case OrderType.DISK:
            aggregate_pr_arr.sort(compareOrderByDisk);
            break;
        case OrderType.MEM:
            aggregate_pr_arr.sort(compareOrderByMem);
            break;
        default:
            aggregate_pr_arr.sort(compareOrderByCpu);
    }

    if (proc_show_num > aggregate_pr_arr.length) {
        proc_show_num = aggregate_pr_arr.length;
    }

    json[0]["PRSUMMARY"] = aggregate_pr_arr.slice(0, proc_show_num);
    json[0]["HPRC"] = hprc;
    delete json[0]["PRC"];
    delete json[0]["PRM"];
    delete json[0]["PRD"];
    delete json[0]["PRG"];
}

function ParseCPUPercentValue(value, percputot) {
    return Math.round(value * 100 / percputot).toFixed(0) + "%";
}

function ParseCPUCurfValue(value) {
    if (value < 1000) {
        return value + "MHz";
    } else {
        fval = value / 1000;
        prefix = 'G';
        if (fval >= 1000) {
            prefix = 'T';
            fval = fval / 1000;
        }

        if (fval < 10) {
            return fval.toFixed(2) + prefix + "Hz";
        } else {
            if (fval < 100) {
                return fval.toFixed(1) + prefix + "Hz";
            } else {
                return fval.toFixed(0) + prefix + "Hz";
            }
        }
    }
}

function ParsePSIPercentValue(value) {
    return Math.round(value).toFixed(0) + "%";
}

function ParseTimeValue(value, hertz, indicatorName) {
    // here is ms
    value = parseInt(value * 1000 / hertz);

    if (value < 100000) {
        return parseInt(value / 1000) + "." + parseInt(value % 1000 / 10) + "s";
    } else {
        //ms to sec
        value = parseInt((value + 500) / 1000);
        if (value < 6000) {
            //sec to min
            return parseInt(value / 60) + "m" + (value % 60) + "s";
        } else {
            //min to hour
            value = parseInt((value + 30) / 60);
            if (value < 6000) {
                return parseInt(value / 60) + "h" + (value % 60) + "m";
            } else {
                // hour to day
                value = parseInt((value + 30) / 60);
                return parseInt(value / 24) + "d" + (value % 24) + "h";
            }
        }
    }
}

function ParseMemValue(ByteValue, indicatorName) {
    var unit = 1024; // byte
    ByteValue = ByteValue - 0;

    var prefix = "";
    if (ByteValue < 0) {
        ByteValue = -ByteValue * 10;
        var prefix = "-";
    }

    if (ByteValue < unit) {
        result = ByteValue + 'B';
    } else if (ByteValue < Math.pow(unit, 2) * 0.8) {
        result = (ByteValue / unit).toFixed(1) + "KB";
    } else if (ByteValue < Math.pow(unit, 3) * 0.8) {
        result = (ByteValue / Math.pow(unit, 2)).toFixed(1) + "MB";
    } else if (ByteValue < Math.pow(unit, 4) * 0.8) {
        result = (ByteValue / Math.pow(unit, 3)).toFixed(1) + "G";
    } else {
        result = (ByteValue / Math.pow(unit, 4)).toFixed(1) + "T";
    }

    return prefix + result;
}

function ParseBandwidth(BandwidthValue, indicatorName) {
    var unit = 1000;

    if (BandwidthValue < unit) {
        return BandwidthValue + ' Kbps';
    } else if (BandwidthValue < Math.pow(unit, 2)) {
        return (BandwidthValue / unit).toFixed(2) + " Mbps";
    } else if (BandwidthValue < Math.pow(unit, 3)) {
        return (BandwidthValue / Math.pow(unit, 2)).toFixed(2) + " Gbps";
    } else {
        return (BandwidthValue / Math.pow(unit, 3)).toFixed(2) + " Tbps";
    }
}

// for number toooo long
function Value2ValueStr(value, indicatorName) {
    // TODO
    return value;
}

function Value2ValuePercent(value, indicatorName) {
    return value + "%";
}

function ArrayToMap(array) {
    map = new Map();
    for (i of array) {
        map.set(i["pid"], i);
    }
    return map;
}

function HtmlToNode(html) {
    let div = document.createElement('div');
    let pos = html.indexOf('<');
    div.innerHTML = html.substring(pos);
    return div.firstChild;
}

function ParseDateToTimesample(itim) {
    ilen = itim.length
    if (ilen != 4 && ilen != 5 && ilen != 12 && ilen != 13) {
        return 0;
    }

    var year, month, day, hour, minute

    if (ilen == 12 || ilen == 13) {
        year = itim.slice(0, 4);
        month = itim.slice(4, 6);
        day = itim.slice(6, 8);
        hour = itim.slice(8, 10);
        minute = itim.slice(-2);
    } else if (ilen == 4 || ilen == 5) {
        now_date = new Date();
        year = now_date.getFullYear();
        month = now_date.getMonth() + 1;
        day = now_date.getDate();

        hour = itim.slice(0, 2);
        minute = itim.slice(-2);
    }

    if (year < 100 || month < 0 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return 0;
    }

    parsed_timestamp = Date.parse(new Date(year, month - 1, day, hour, minute)) / 1000;
    return parsed_timestamp;
}

Date.prototype.format = function (fmt) {
    let ret;
    const opt = {
        "Y+": this.getFullYear().toString(),
        "M+": (this.getMonth() + 1).toString(),
        "D+": this.getDate().toString(),
        "h+": this.getHours().toString(),
        "m+": this.getMinutes().toString(),
    };
    for (let k in opt) {
        ret = new RegExp("(" + k + ")").exec(fmt);
        if (ret) {
            fmt = fmt.replace(ret[1], (ret[1].length == 1) ? (opt[k]) : (opt[k].padStart(ret[1].length, "0")))
        };
    };
    return fmt;
}