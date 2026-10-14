/*
 * Unit tests for json.c (atop sample -> JSON serialisation).
 *
 * These tests build synthetic (but structurally valid) `struct sstat` and
 * `struct devtstat` values and drive jsonout() with the "ALL" label set so
 * that every per-subsystem printer runs, including the ones that only have
 * data on special hardware (GPU, InfiniBand, NUMA, LLC/RDT, LVM, MD, NFS).
 * This exercises those code paths deterministically on any machine, without
 * depending on the host actually having that hardware.
 *
 * The fixtures live in static storage and are reset for every case, so the
 * tests never allocate heap memory and there is nothing to leak.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#include "test_framework.h"
#include "config.h"
#include "atop.h"
#include "photosyst.h"
#include "photoproc.h"
#include "output.h"

/*
 * Declare the atophttpd jsonout() prototype directly: the project's json.h
 * and atop's bundled json.h share a name but have different signatures, and
 * from tests/unit/ the include search order would pick up the wrong one.
 */
int jsonout(int, char *, time_t, int, struct devtstat *, struct sstat *,
            int, unsigned int, char, connection *);

/* globals normally provided by httpd.c */
int __debug = 0;
unsigned int pagesize = 4096;
unsigned short hertz = 100;
struct utsname utsname;
int hidecmdline = 0;
struct output defop = { .output_type = OUTPUT_BUF, .done = NULL };

/* NETATOP / GPUSTAT live in atop.h and gate the PRN / PRE printers. */

/*
 * Statically-allocated fixtures. Using static storage (rather than malloc)
 * keeps every test allocation-free, so there is nothing to leak.
 */
static struct sstat    g_ss;
static struct tstat    g_tasks[4];
static struct devtstat g_dt;

static struct sstat *make_sstat(int cpu0_has_freq)
{
	struct sstat *ss = &g_ss;
	memset(ss, 0, sizeof(*ss));

	/* CPU: two cpus, so json_calc_freqscale is driven with different
	 * combinations of maxfreq / ticks / cnt across the two invocations. */
	ss->cpu.nrcpu = 2;
	if (cpu0_has_freq) {
		ss->cpu.cpu[0].freqcnt.maxfreq = 3000;
		ss->cpu.cpu[0].freqcnt.cnt = 6000;
		ss->cpu.cpu[0].freqcnt.ticks = 2;   /* maxfreq && ticks */
		ss->cpu.cpu[1].freqcnt.maxfreq = 3000;
		ss->cpu.cpu[1].freqcnt.cnt = 2500;
		ss->cpu.cpu[1].freqcnt.ticks = 0;   /* maxfreq, no ticks */
		ss->cpu.all.instr = 1;              /* triggers instr/cycle reset */
		ss->cpu.all.cycle = 1;
	} else {
		ss->cpu.cpu[0].freqcnt.maxfreq = 0;
		ss->cpu.cpu[0].freqcnt.cnt = 1500;  /* no maxfreq, cnt set */
		ss->cpu.cpu[0].freqcnt.ticks = 0;
		ss->cpu.cpu[1].freqcnt.maxfreq = 0;
		ss->cpu.cpu[1].freqcnt.cnt = 0;     /* nothing known */
		ss->cpu.cpu[1].freqcnt.ticks = 0;
	}

	/* CPL / MEM / SWP / PAG just read scalar fields - give them something. */
	ss->cpu.lavg1 = 0.5;
	ss->mem.physmem = 1000;
	ss->mem.shugepagesz = 2048 * 1024;

	/* GPU */
	ss->gpu.nrgpus = 2;
	strcpy(ss->gpu.gpu[0].busid, "0000:01:00");
	strcpy(ss->gpu.gpu[0].type, "TestGPU");
	ss->gpu.gpu[0].gpupercnow = 42;
	strcpy(ss->gpu.gpu[1].busid, "0000:02:00");
	strcpy(ss->gpu.gpu[1].type, "TestGPU2");
	ss->gpu.gpu[1].gpupercnow = 7;

	/* PSI present (overridable per case). */
	ss->psi.present = 1;

	/* LVM / MDD / DSK - loops terminate on an empty name[0]; use two each
	 * so the inter-element separators (i>0) run too. */
	strcpy(ss->dsk.lvm[0].name, "vg-root");
	strcpy(ss->dsk.lvm[1].name, "vg-data");
	strcpy(ss->dsk.mdd[0].name, "md0");
	strcpy(ss->dsk.mdd[1].name, "md1");
	strcpy(ss->dsk.dsk[0].name, "sda");
	strcpy(ss->dsk.dsk[1].name, "sdb");

	/* NFS mounts + client/server counters */
	ss->nfs.nfsmounts.nrmounts = 2;
	strcpy(ss->nfs.nfsmounts.nfsmnt[0].mountdev, "server:/export");
	strcpy(ss->nfs.nfsmounts.nfsmnt[1].mountdev, "server:/home");
	ss->nfs.client.rpccnt = 7;
	ss->nfs.server.rpccnt = 9;

	/* NET general + two interfaces */
	ss->net.tcp.InSegs = 123;
	strcpy(ss->intf.intf[0].name, "eth0");
	ss->intf.intf[0].rpack = 10;
	strcpy(ss->intf.intf[1].name, "eth1");
	ss->intf.intf[1].rpack = 20;

	/* InfiniBand */
	ss->ifb.nrports = 2;
	strcpy(ss->ifb.ifb[0].ibname, "mlx5_0");
	ss->ifb.ifb[0].portnr = 1;
	ss->ifb.ifb[0].lanes = 4;
	strcpy(ss->ifb.ifb[1].ibname, "mlx5_1");
	ss->ifb.ifb[1].portnr = 1;
	ss->ifb.ifb[1].lanes = 4;

	/* NUMA memory + cpu */
	ss->memnuma.nrnuma = 2;
	ss->memnuma.numa[0].totmem = 500;
	ss->memnuma.numa[1].totmem = 600;
	ss->cpunuma.nrnuma = 2;
	ss->cpunuma.numa[0].numanr = 0;
	ss->cpunuma.numa[0].nrcpu = 2;
	ss->cpunuma.numa[1].numanr = 1;
	ss->cpunuma.numa[1].nrcpu = 2;

	/* LLC / RDT */
	ss->llc.nrllcs = 2;
	ss->llc.perllc[0].id = 0;
	ss->llc.perllc[0].occupancy = 0.5;
	ss->llc.perllc[1].id = 1;
	ss->llc.perllc[1].occupancy = 0.25;

	return ss;
}

static struct devtstat *make_devtstat(void)
{
	struct devtstat *dt = &g_dt;
	struct tstat *tasks = g_tasks;

	memset(dt, 0, sizeof(*dt));
	memset(tasks, 0, sizeof(g_tasks));

	/* task 0: a real process, killed by a signal with a core dump, and a
	 * name containing characters that must be escaped to '#'. */
	tasks[0].gen.isproc = 1;
	tasks[0].gen.pid = 100;
	tasks[0].gen.tgid = 100;
	strcpy(tasks[0].gen.name, "pr\"oc\\x");
	strcpy(tasks[0].gen.cmdline, "cmd \"arg\" \\path");
	tasks[0].gen.excode = (int)0x80000000 | 0x80 | 0x05; /* N, core, signal */
	tasks[0].gen.state = 'R';
	strcpy(tasks[0].gen.utsname, "abc123");
	tasks[0].mem.pmem = (unsigned long long)-1LL;        /* -> 0 in PRM */
	tasks[0].gpu.state = 'A';                             /* active GPU use */
	tasks[0].gpu.nrgpus = 1;

	/* task 1: a process killed by a signal WITHOUT a core dump (st 'S'). */
	tasks[1].gen.isproc = 1;
	tasks[1].gen.pid = 150;
	tasks[1].gen.tgid = 150;
	strcpy(tasks[1].gen.name, "signalled");
	strcpy(tasks[1].gen.cmdline, "signalled");
	tasks[1].gen.excode = 0x09;                           /* signal 9, no core */
	tasks[1].gen.state = 'E';

	/* task 2: a process that exited normally (exit code path). */
	tasks[2].gen.isproc = 1;
	tasks[2].gen.pid = 200;
	tasks[2].gen.tgid = 200;
	strcpy(tasks[2].gen.name, "clean");
	strcpy(tasks[2].gen.cmdline, "clean --run");
	tasks[2].gen.excode = 0x0100;                         /* exit code 1 */
	tasks[2].gen.state = 'S';

	/* task 3: a thread (pid==tgid, !isproc) -> skipped by the PR* loops. */
	tasks[3].gen.isproc = 0;
	tasks[3].gen.pid = 201;
	tasks[3].gen.tgid = 201;
	strcpy(tasks[3].gen.name, "thread");

	dt->taskall = tasks;
	dt->ntaskall = 4;
	dt->nprocall = 3;
	dt->nprocactive = 3;
	return dt;
}

/* A single jsonout() invocation described declaratively. */
struct json_tc {
	int         cpu0_has_freq;
	int         flags;
	const char *labels;
	int         hidecmd;
	int         psi_present;
	time_t      ts;
	int         expect_ok;    /* 1: ret must be 0; 0: ret must be non-zero */
	int         check_offset; /* also assert the OUTPUT_BUF sink was reset */
};

static void run_json_tc(const struct json_tc *tc)
{
	struct sstat *ss = make_sstat(tc->cpu0_has_freq);
	ss->psi.present = tc->psi_present;
	struct devtstat *dt = make_devtstat();

	char labels[64];
	snprintf(labels, sizeof(labels), "%s", tc->labels);

	hidecmdline = tc->hidecmd;
	int ret = jsonout(tc->flags, labels, tc->ts, 1, dt, ss, 0, 0, 0, NULL);
	hidecmdline = 0;

	if (tc->expect_ok)
		CHECK_EQ_INT(ret, 0);
	else
		CHECK(ret != 0);

	if (tc->check_offset)
		CHECK_EQ_INT(defop.ob.offset, 0);
}

/* The "ALL" label set with PRN + PRE enabled: exercises every printer. */
static void test_jsonout_all_labels(void)
{
	const struct json_tc tc = {
		.cpu0_has_freq = 1, .flags = NETATOP | GPUSTAT, .labels = "ALL",
		.psi_present = 1, .ts = 1234567890, .expect_ok = 1, .check_offset = 1,
	};
	run_json_tc(&tc);
}

/* Second configuration to cover the remaining json_calc_freqscale branches
 * (no maxfreq) and the per-cpu "nothing known" case. */
static void test_jsonout_freqscale_variants(void)
{
	const struct json_tc tc = {
		.cpu0_has_freq = 0, .labels = "CPU,cpu", .psi_present = 1,
		.ts = 1, .expect_ok = 1,
	};
	run_json_tc(&tc);
}

/* hidecmdline replaces every command line with "***". */
static void test_jsonout_hidecmdline(void)
{
	const struct json_tc tc = {
		.cpu0_has_freq = 1, .labels = "PRG,PRC,PRM", .hidecmd = 1,
		.psi_present = 1, .ts = 1, .expect_ok = 1,
	};
	run_json_tc(&tc);
}

/* A single valid label still produces a full document. */
static void test_jsonout_single_label(void)
{
	const struct json_tc tc = {
		.cpu0_has_freq = 1, .labels = "MEM", .psi_present = 1,
		.ts = 1, .expect_ok = 1,
	};
	run_json_tc(&tc);
}

/* Error path: label list starting with '-'. */
static void test_jsonout_leading_dash(void)
{
	const struct json_tc tc = {
		.cpu0_has_freq = 1, .labels = "-CPU", .psi_present = 1, .ts = 1,
	};
	run_json_tc(&tc);
}

/* Error path: an unknown label. */
static void test_jsonout_unknown_label(void)
{
	const struct json_tc tc = {
		.cpu0_has_freq = 1, .labels = "CPU,NOPE", .psi_present = 1, .ts = 1,
	};
	run_json_tc(&tc);
}

/* PSI printer short-circuits when pressure stats are not supported. */
static void test_jsonout_psi_absent(void)
{
	const struct json_tc tc = {
		.cpu0_has_freq = 1, .labels = "PSI", .psi_present = 0,
		.ts = 1, .expect_ok = 1,
	};
	run_json_tc(&tc);
}

/* PRN/PRE printers short-circuit when the netatop / GPU flags are absent. */
static void test_jsonout_prn_pre_disabled(void)
{
	const struct json_tc tc = {
		.cpu0_has_freq = 1, .flags = 0, .labels = "PRN,PRE",
		.psi_present = 1, .ts = 1, .expect_ok = 1,
	};
	run_json_tc(&tc);
}

int main(void)
{
	uname(&utsname);
	TEST_SUITE_BEGIN("json");
	RUN_TEST(test_jsonout_all_labels);
	RUN_TEST(test_jsonout_freqscale_variants);
	RUN_TEST(test_jsonout_hidecmdline);
	RUN_TEST(test_jsonout_single_label);
	RUN_TEST(test_jsonout_leading_dash);
	RUN_TEST(test_jsonout_unknown_label);
	RUN_TEST(test_jsonout_psi_absent);
	RUN_TEST(test_jsonout_prn_pre_disabled);
	TEST_SUITE_END();
}
