function compareOrderByCpu(a, b) {
    acpu = a["utime"] + a["stime"];
    bcpu = b["utime"] + b["stime"];
    if (acpu > bcpu) {
        return -1;
    }
    if (acpu < bcpu) {
        return 1;
    }
    return 0;
}

function compareOrderByDisk(a, b) {
    if (a["wsz"] > a["cwsz"]) {
        adsk = a["rio"] + a["wsz"] - a["cwsz"];
    } else {
        adsk = a["rio"];
    }

    if (b["wsz"] > b["cwsz"]) {
        bdsk = b["rio"] + b["wsz"] - b["cwsz"];
    } else {
        bdsk = b["rio"];
    }

    if (adsk > bdsk) {
        return -1;
    }

    if (adsk < bdsk) {
        return 1;
    }

    return compareOrderByCpu(a, b);
}

function compareOrderByDiskIoms(a, b) {
    adsk_value = a["io_ms"]
    bdsk_value = b["io_ms"]

    if (adsk_value > bdsk_value) {
        return -1;
    }

    if (adsk_value < bdsk_value) {
        return 1;
    }

    return 0;
}

function compareOrderByMem(a, b) {
    amem = a["rmem"];
    bmem = b["rmem"];

    if (amem > bmem) {
        return -1;
    }
    if (amem < bmem) {
        return 1;
    }

    return 0;
}

function compareOrderByNet(a, b) {
    anet_value = a["rpack"] + a["spack"]
    bnet_value = b["rpack"] + b["bpack"]

    if (anet_value > bnet_value) {
        return -1;
    }

    if (anet_value < bnet_value) {
        return 1;
    }

    return 0;
}

function compareOrderLLC(a, b) {
    aLLC = a["occupancy"] * 1
    bLLC = b["occupancy"] * 1

    if (aLLC > bLLC) {
        return -1;
    }

    if (aLLC < bLLC) {
        return 1;
    }

    return 0;
}
