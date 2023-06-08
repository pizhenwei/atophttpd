/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *   Fei Li <lifei.shirley@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>

#include "config.h"
#include "atop.h"
#include "json.h"
#include "output.h"

#define LEN_HP_SIZE	64
#define LINE_BUF_SIZE	1024

static void json_print_CPU();
static void json_print_cpu();
static void json_print_CPL();
static void json_print_GPU();
static void json_print_MEM();
static void json_print_SWP();
static void json_print_PAG();
static void json_print_PSI();
static void json_print_LVM();
static void json_print_MDD();
static void json_print_DSK();
static void json_print_NFM();
static void json_print_NFC();
static void json_print_NFS();
static void json_print_NET();
static void json_print_IFB();
static void json_print_NUM();
static void json_print_NUC();
static void json_print_LLC();
static void json_print_PRG();
static void json_print_PRC();
static void json_print_PRM();
static void json_print_PRD();
static void json_print_PRN();
static void json_print_PRE();

/*
** table with possible labels and the corresponding
** print-function for json style output
*/
struct labeldef {
	char *label;
	int valid;
	void (*prifunc)(int, char *, struct sstat *, struct tstat *, int);
};

extern struct output defop;

static int jsondef(char *pd, struct labeldef *labeldef, int numlabels)
{
	int i;
	char		*p, *ep = pd + strlen(pd);

	if (*pd == '-') {
		char *err =  "json lables should be followed by label list\n";
		output_samp(&defop, err, strlen(err));
		return -EINVAL;
	}

	while (pd < ep) {
		/*
		** exchange comma by null-byte
		*/
		if ((p = strchr(pd, ',')))
			*p = 0;
		else
			p = ep - 1;

		/*
		** check if the next label exists
		*/
		for (i = 0; i < numlabels; i++)
		{
			if (!strcmp(labeldef[i].label, pd)) {
				labeldef[i].valid = 1;
				break;
			}
		}

		if (i == numlabels)
		{
			if (!strcmp("ALL", pd)) {
				for (i = 0; i < numlabels; i++)
					labeldef[i].valid = 1;
				break;
			} else {
				char err[64];
				snprintf(err, sizeof(err), "json lables not supported: %s\n", pd);
				output_samp(&defop, err, strlen(err));
				return -EINVAL;
			}
		}

		pd = p + 1;
	}

	return 0;
}

int jsonout(int flags, char *pd, time_t curtime, int numsecs,
         struct devtstat *devtstat, struct sstat *sstat,
         int nexit, unsigned int noverflow, char flag, connection *conn)
{
	char header[256], general[256];
	struct tstat *tmp = devtstat->taskall;
	int buflen = 0;
	int i, ret;

	struct labeldef	labeldef[] = {
		{ "CPU",	0,	json_print_CPU },
		{ "cpu",	0,	json_print_cpu },
		{ "CPL",	0,	json_print_CPL },
		{ "GPU",	0,	json_print_GPU },
		{ "MEM",	0,	json_print_MEM },
		{ "SWP",	0,	json_print_SWP },
		{ "PAG",	0,	json_print_PAG },
		{ "PSI",	0,	json_print_PSI },
		{ "LVM",	0,	json_print_LVM },
		{ "MDD",	0,	json_print_MDD },
		{ "DSK",	0,	json_print_DSK },
		{ "NFM",	0,	json_print_NFM },
		{ "NFC",	0,	json_print_NFC },
		{ "NFS",	0,	json_print_NFS },
		{ "NET",	0,	json_print_NET },
		{ "IFB",	0,	json_print_IFB },
		{ "NUM",	0,	json_print_NUM },
		{ "NUC",	0,	json_print_NUC },
		{ "LLC",	0,	json_print_LLC },

		{ "PRG",	0,	json_print_PRG },
		{ "PRC",	0,	json_print_PRC },
		{ "PRM",	0,	json_print_PRM },
		{ "PRD",	0,	json_print_PRD },
		{ "PRN",	0,	json_print_PRN },
		{ "PRE",	0,	json_print_PRE },
	};

	int numlabels = sizeof(labeldef) / sizeof(struct labeldef);

	ret = jsondef(pd, labeldef, numlabels);
	if (ret) {
		output_samp_done(&defop, conn);
		return ret;
	}

	buflen = snprintf(general, sizeof(general),
			"{\"host\": \"%s\", "
			"\"timestamp\": %ld, "
			"\"elapsed\": %d",
			utsname.nodename,
			curtime,
			numsecs
			);

	output_samp(&defop, general, buflen);

	/* Replace " with # in case json can not parse this out */
	for (int k = 0; k < devtstat->ntaskall; k++, tmp++) {
		for (int j = 0; (j < sizeof(tmp->gen.name)) && tmp->gen.name[j]; j++)
			if ((tmp->gen.name[j] == '\"') || (tmp->gen.name[j] == '\\'))
				tmp->gen.name[j] = '#';

		if (hidecmdline) {
			strcpy(tmp->gen.cmdline, "***");
			continue;
		}

		for (int j = 0; (j < sizeof(tmp->gen.cmdline) && tmp->gen.cmdline[j]); j++)
			if ((tmp->gen.cmdline[j] == '\"') || (tmp->gen.cmdline[j] == '\\'))
				tmp->gen.cmdline[j] = '#';
	}

	for (i = 0; i < numlabels; i++) {
		if (!labeldef[i].valid)
			continue;

		/* prepare generic columns */
		snprintf(header, sizeof header, "\"%s\"",
				labeldef[i].label);
		/* call all print-functions */
		(labeldef[i].prifunc)(flags, header, sstat, devtstat->taskall, devtstat->ntaskall);
	}

	output_samp(&defop, "}\n", 2);
	output_samp_done(&defop, conn);

	return 0;
}

/*
** print functions for system-level statistics
*/
static void
json_calc_freqscale(count_t maxfreq, count_t cnt, count_t ticks,
               count_t *freq, int *freqperc)
{
	// if ticks != 0,do full calcs
	if (maxfreq && ticks) {
		*freq = cnt / ticks;
		*freqperc = 100* *freq / maxfreq;
	} else if (maxfreq) { // max frequency is known so % can be calculated
		*freq = cnt;
		*freqperc = 100*cnt/maxfreq;
	} else if (cnt) {   // no max known, set % to 100
		*freq = cnt;
		*freqperc = 100;
	} else {            // nothing is known: set freq to 0, % to 100
		*freq = 0;
		*freqperc = 100;
	}
}

static void json_print_CPU(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	count_t maxfreq = 0;
	count_t cnt = 0;
	count_t ticks = 0;
	count_t freq;
	int freqperc;
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	// calculate average clock frequency
	for (i = 0; i < ss->cpu.nrcpu; i++) {
		cnt += ss->cpu.cpu[i].freqcnt.cnt;
		ticks += ss->cpu.cpu[i].freqcnt.ticks;
	}
	maxfreq = ss->cpu.cpu[0].freqcnt.maxfreq;
	json_calc_freqscale(maxfreq, cnt, ticks, &freq, &freqperc);

	if (ss->cpu.all.instr == 1) {
		ss->cpu.all.instr = 0;
		ss->cpu.all.cycle = 0;
	}

	buflen = snprintf(buf, sizeof(buf), ", %s: {"
		"\"hertz\": %u, "
		"\"nrcpu\": %lld, "
		"\"stime\": %lld, "
		"\"utime\": %lld, "
		"\"ntime\": %lld, "
		"\"itime\": %lld, "
		"\"wtime\": %lld, "
		"\"Itime\": %lld, "
		"\"Stime\": %lld, "
		"\"steal\": %lld, "
		"\"guest\": %lld, "
		"\"freq\": %lld, "
		"\"freqperc\": %d, "
		"\"instr\": %lld, "
		"\"cycle\": %lld}",
		hp,
		hertz,
		ss->cpu.nrcpu,
		ss->cpu.all.stime,
		ss->cpu.all.utime,
		ss->cpu.all.ntime,
		ss->cpu.all.itime,
		ss->cpu.all.wtime,
		ss->cpu.all.Itime,
		ss->cpu.all.Stime,
		ss->cpu.all.steal,
		ss->cpu.all.guest,
		freq,
		freqperc,
		ss->cpu.all.instr,
		ss->cpu.all.cycle
		);

	output_samp(&defop, buf, buflen);
}

static void json_print_cpu(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	count_t maxfreq = 0;
	count_t cnt = 0;
	count_t ticks = 0;
	count_t freq;
	int freqperc;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	char br[LEN_HP_SIZE];
	buflen = sprintf(br, ", %s: [", hp);
	output_samp(&defop, br, buflen);

	for (i = 0; i < ss->cpu.nrcpu; i++) {
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		cnt = ss->cpu.cpu[i].freqcnt.cnt;
		ticks = ss->cpu.cpu[i].freqcnt.ticks;
		maxfreq= ss->cpu.cpu[0].freqcnt.maxfreq;

		json_calc_freqscale(maxfreq, cnt, ticks, &freq, &freqperc);

		buflen = snprintf(buf, sizeof(buf), "{\"cpuid\": %d, "
			"\"stime\": %lld, "
			"\"utime\": %lld, "
			"\"ntime\": %lld, "
			"\"itime\": %lld, "
			"\"wtime\": %lld, "
			"\"Itime\": %lld, "
			"\"Stime\": %lld, "
			"\"steal\": %lld, "
			"\"guest\": %lld, "
			"\"freq\": %lld, "
			"\"freqperc\": %d, "
			"\"instr\": %lld, "
			"\"cycle\": %lld}",
			i,
			ss->cpu.cpu[i].stime,
			ss->cpu.cpu[i].utime,
			ss->cpu.cpu[i].ntime,
			ss->cpu.cpu[i].itime,
			ss->cpu.cpu[i].wtime,
			ss->cpu.cpu[i].Itime,
			ss->cpu.cpu[i].Stime,
			ss->cpu.cpu[i].steal,
			ss->cpu.cpu[i].guest,
			freq,
			freqperc,
			ss->cpu.cpu[i].instr,
			ss->cpu.cpu[i].cycle
			);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_CPL(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	buflen = snprintf(buf, sizeof(buf), ", %s: {"
		"\"lavg1\": %.2f, "
		"\"lavg5\": %.2f, "
		"\"lavg15\": %.2f, "
		"\"csw\": %lld, "
		"\"devint\": %lld}",
		hp,
		ss->cpu.lavg1,
		ss->cpu.lavg5,
		ss->cpu.lavg15,
		ss->cpu.csw,
		ss->cpu.devint);

	output_samp(&defop, buf, buflen);
}

static void json_print_GPU(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	char br[LEN_HP_SIZE];
	buflen = sprintf(br, ", %s: [", hp);
	output_samp(&defop, br, buflen);

	for (i = 0; i < ss->gpu.nrgpus; i++) {
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"gpuid\": %d, "
			"\"busid\": \"%.19s\", "
			"\"type\": \"%.19s\", "
			"\"gpupercnow\": %d, "
			"\"mempercnow\": %d, "
			"\"memtotnow\": %lld, "
			"\"memusenow\": %lld, "
			"\"samples\": %lld, "
			"\"gpuperccum\": %lld, "
			"\"memperccum\": %lld, "
			"\"memusecum\": %lld}",
			i,
			ss->gpu.gpu[i].busid,
			ss->gpu.gpu[i].type,
			ss->gpu.gpu[i].gpupercnow,
			ss->gpu.gpu[i].mempercnow,
			ss->gpu.gpu[i].memtotnow,
			ss->gpu.gpu[i].memusenow,
			ss->gpu.gpu[i].samples,
			ss->gpu.gpu[i].gpuperccum,
			ss->gpu.gpu[i].memperccum,
			ss->gpu.gpu[i].memusecum);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_MEM(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	buflen = snprintf(buf, sizeof(buf), ", %s: {"
		"\"physmem\": %lld, "
		"\"freemem\": %lld, "
		"\"cachemem\": %lld, "
		"\"buffermem\": %lld, "
		"\"slabmem\": %lld, "
		"\"cachedrt\": %lld, "
		"\"slabreclaim\": %lld, "
		"\"vmwballoon\": %lld, "
		"\"shmem\": %lld, "
		"\"shmrss\": %lld, "
		"\"shmswp\": %lld, "
		"\"pagetables\": %lld, "
		"\"hugepagesz\": %lld, "
		"\"tothugepage\": %lld, "
		"\"freehugepage\": %lld, "
		"\"tcpsk\": %lld, "
		"\"udpsk\": %lld}",
		hp,
		ss->mem.physmem * pagesize,
		ss->mem.freemem * pagesize,
		ss->mem.cachemem * pagesize,
		ss->mem.buffermem * pagesize,
		ss->mem.slabmem * pagesize,
		ss->mem.cachedrt * pagesize,
		ss->mem.slabreclaim * pagesize,
		ss->mem.vmwballoon * pagesize,
		ss->mem.shmem * pagesize,
		ss->mem.shmrss * pagesize,
		ss->mem.shmswp * pagesize,
		ss->mem.pagetables * pagesize,
		ss->mem.hugepagesz,
		ss->mem.tothugepage,
		ss->mem.freehugepage,
		ss->mem.tcpsock * pagesize,
		ss->mem.udpsock * pagesize);

	output_samp(&defop, buf, buflen);
}

static void json_print_SWP(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	buflen = snprintf(buf, sizeof(buf), ", %s: {"
		"\"totswap\": %lld, "
		"\"freeswap\": %lld, "
		"\"swcac\": %lld, "
		"\"committed\": %lld, "
		"\"commitlim\": %lld}",
		hp,
		ss->mem.totswap * pagesize,
		ss->mem.freeswap * pagesize,
		ss->mem.swapcached * pagesize,
		ss->mem.committed * pagesize,
		ss->mem.commitlim * pagesize);

	output_samp(&defop, buf, buflen);
}

static void json_print_PAG(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	buflen = snprintf(buf, sizeof(buf), ", %s: {"
		"\"stall\": %lld, "
		"\"compacts\": %lld, "
		"\"numamigs\": %lld, "
		"\"migrates\": %lld, "
		"\"pgscans\": %lld, "
		"\"pgsteal\": %lld,"
		"\"allocstall\": %lld, "
		"\"pgins\": %lld, "
		"\"pgouts\": %lld, "
		"\"swins\": %lld, "
		"\"swouts\": %lld, "
		"\"oomkills\": %lld}",
		hp,
		ss->mem.allocstall,
		ss->mem.compactstall,
		ss->mem.numamigrate,
		ss->mem.pgmigrate,
		ss->mem.pgscans,
		ss->mem.pgsteal,
		ss->mem.allocstall,
		ss->mem.pgins,
		ss->mem.pgouts,
		ss->mem.swins,
		ss->mem.swouts,
		ss->mem.oomkills);

	output_samp(&defop, buf, buflen);
}

static void json_print_PSI(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	if ( !(ss->psi.present) )
		return;

	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	buflen = snprintf(buf, sizeof(buf), ", %s: {"
		"\"psi\": \"%c\", "
		"\"cs10\": %.1f, "
		"\"cs60\": %.1f, "
		"\"cs300\": %.1f, "
		"\"cstot\": %llu, "
		"\"ms10\": %.1f, "
		"\"ms60\": %.1f, "
		"\"ms300\": %.1f, "
		"\"mstot\": %llu, "
		"\"mf10\": %.1f, "
		"\"mf60\": %.1f, "
		"\"mf300\": %.1f, "
		"\"mftot\": %llu, "
		"\"ios10\": %.1f, "
		"\"ios60\": %.1f, "
		"\"ios300\": %.1f, "
		"\"iostot\": %llu, "
		"\"iof10\": %.1f, "
		"\"iof60\": %.1f, "
		"\"iof300\": %.1f, "
		"\"ioftot\": %llu}",
		hp, ss->psi.present ? 'y' : 'n',
		ss->psi.cpusome.avg10, ss->psi.cpusome.avg60,
		ss->psi.cpusome.avg300, ss->psi.cpusome.total,
		ss->psi.memsome.avg10, ss->psi.memsome.avg60,
		ss->psi.memsome.avg300, ss->psi.memsome.total,
		ss->psi.memfull.avg10, ss->psi.memfull.avg60,
		ss->psi.memfull.avg300, ss->psi.memfull.total,
		ss->psi.iosome.avg10, ss->psi.iosome.avg60,
		ss->psi.iosome.avg300, ss->psi.iosome.total,
		ss->psi.iofull.avg10, ss->psi.iofull.avg60,
		ss->psi.iofull.avg300, ss->psi.iofull.total);

	output_samp(&defop, buf, buflen);
}

static void json_print_LVM(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	char br[LEN_HP_SIZE];
	buflen = sprintf(br, ", %s: [", hp);
	output_samp(&defop, br, buflen);

	for (i = 0; ss->dsk.lvm[i].name[0]; i++) {
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"lvmname\": \"%.19s\", "
			"\"io_ms\": %lld, "
			"\"nread\": %lld, "
			"\"ndiscrd\": %lld, "
			"\"nrsect\": %lld, "
			"\"nwrite\": %lld, "
			"\"nwsect\": %lld, "
			"\"avque\": %lld, "
			"\"inflight\": %lld}",
			ss->dsk.lvm[i].name,
			ss->dsk.lvm[i].io_ms,
			ss->dsk.lvm[i].nread,
			ss->dsk.lvm[i].ndisc,
			ss->dsk.lvm[i].nrsect,
			ss->dsk.lvm[i].nwrite,
			ss->dsk.lvm[i].nwsect,
			ss->dsk.lvm[i].avque,
			ss->dsk.lvm[i].inflight);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_MDD(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	char br[LEN_HP_SIZE];
	buflen = sprintf(br, ", %s: [", hp);
	output_samp(&defop, br, buflen);

	for (i = 0; ss->dsk.mdd[i].name[0]; i++) {
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"mddname\": \"%.19s\", "
			"\"io_ms\": %lld, "
			"\"nread\": %lld, "
			"\"nrsect\": %lld, "
			"\"nwrite\": %lld, "
			"\"nwsect\": %lld, "
			"\"avque\": %lld, "
			"\"inflight\": %lld}",
			ss->dsk.mdd[i].name,
			ss->dsk.mdd[i].io_ms,
			ss->dsk.mdd[i].nread,
			ss->dsk.mdd[i].nrsect,
			ss->dsk.mdd[i].nwrite,
			ss->dsk.mdd[i].nwsect,
			ss->dsk.mdd[i].avque,
			ss->dsk.mdd[i].inflight);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_DSK(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

        char br[LEN_HP_SIZE];
        buflen = sprintf(br, ", %s: [", hp);
        output_samp(&defop, br, buflen);

	for (i = 0; ss->dsk.dsk[i].name[0]; i++) {
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"dskname\": \"%.19s\", "
			"\"io_ms\": %lld, "
			"\"nread\": %lld, "
			"\"nrsect\": %lld, "
			"\"ndiscrd\": %lld, "
			"\"nwrite\": %lld, "
			"\"nwsect\": %lld, "
			"\"avque\": %lld, "
			"\"inflight\": %lld}",
			ss->dsk.dsk[i].name,
			ss->dsk.dsk[i].io_ms,
			ss->dsk.dsk[i].nread,
			ss->dsk.dsk[i].nrsect,
			ss->dsk.dsk[i].ndisc,
			ss->dsk.dsk[i].nwrite,
			ss->dsk.dsk[i].nwsect,
			ss->dsk.dsk[i].avque,
			ss->dsk.dsk[i].inflight);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_NFM(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

        char br[LEN_HP_SIZE];
        buflen = sprintf(br, ", %s: [", hp);
        output_samp(&defop, br, buflen);

	for (i = 0; i < ss->nfs.nfsmounts.nrmounts; i++) {
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"mountdev\": \"%.19s\", "
			"\"bytestotread\": %lld, "
			"\"bytestotwrite\": %lld, "
			"\"bytesread\": %lld, "
			"\"byteswrite\": %lld, "
			"\"bytesdread\": %lld, "
			"\"bytesdwrite\": %lld, "
			"\"pagesmread\": %lld, "
			"\"pagesmwrite\": %lld}",
			ss->nfs.nfsmounts.nfsmnt[i].mountdev,
			ss->nfs.nfsmounts.nfsmnt[i].bytestotread,
			ss->nfs.nfsmounts.nfsmnt[i].bytestotwrite,
			ss->nfs.nfsmounts.nfsmnt[i].bytesread,
			ss->nfs.nfsmounts.nfsmnt[i].byteswrite,
			ss->nfs.nfsmounts.nfsmnt[i].bytesdread,
			ss->nfs.nfsmounts.nfsmnt[i].bytesdwrite,
			ss->nfs.nfsmounts.nfsmnt[i].pagesmread * pagesize,
			ss->nfs.nfsmounts.nfsmnt[i].pagesmwrite * pagesize);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_NFC(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	buflen = snprintf(buf, sizeof(buf), ", %s: {"
		"\"rpccnt\": %lld, "
		"\"rpcread\": %lld, "
		"\"rpcwrite\": %lld, "
		"\"rpcretrans\": %lld, "
		"\"rpcautrefresh\": %lld}",
		hp,
		ss->nfs.client.rpccnt,
		ss->nfs.client.rpcread,
		ss->nfs.client.rpcwrite,
		ss->nfs.client.rpcretrans,
		ss->nfs.client.rpcautrefresh);

	output_samp(&defop, buf, buflen);
}

static void json_print_NFS(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	buflen = snprintf(buf, sizeof(buf), ", %s: {"
		"\"rpccnt\": %lld, "
		"\"rpcread\": %lld, "
		"\"rpcwrite\": %lld, "
		"\"nrbytes\": %lld, "
		"\"nwbytes\": %lld, "
		"\"rpcbadfmt\": %lld, "
		"\"rpcbadaut\": %lld, "
		"\"rpcbadcln\": %lld, "
		"\"netcnt\": %lld, "
		"\"nettcpcnt\": %lld, "
		"\"netudpcnt\": %lld, "
		"\"nettcpcon\": %lld, "
		"\"rchits\": %lld, "
		"\"rcmiss\": %lld, "
		"\"rcnocache\": %lld}",
		hp,
		ss->nfs.server.rpccnt,
		ss->nfs.server.rpcread,
		ss->nfs.server.rpcwrite,
		ss->nfs.server.nrbytes,
		ss->nfs.server.nwbytes,
		ss->nfs.server.rpcbadfmt,
		ss->nfs.server.rpcbadaut,
		ss->nfs.server.rpcbadcln,
		ss->nfs.server.netcnt,
		ss->nfs.server.nettcpcnt,
		ss->nfs.server.netudpcnt,
		ss->nfs.server.nettcpcon,
		ss->nfs.server.rchits,
		ss->nfs.server.rcmiss,
		ss->nfs.server.rcnoca);

	output_samp(&defop, buf, buflen);
}

static void json_print_NET(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	buflen = snprintf(buf, sizeof(buf), ", \"NET_GENERAL\": {"
		"\"rpacketsTCP\": %lld, "
		"\"spacketsTCP\": %lld, "
		"\"inerrTCP\": %lld, "
		"\"oresetTCP\": %lld, "
		"\"activeOpensTCP\": %lld, "
		"\"passiveOpensTCP\": %lld, "
		"\"retransSegsTCP\": %lld, "
		"\"noportUDP\": %lld, "
		"\"inerrUDP\": %lld, "
		"\"rpacketsUDP\": %lld, "
		"\"spacketsUDP\": %lld, "
		"\"rpacketsIP\": %lld, "
		"\"spacketsIP\": %lld, "
		"\"dpacketsIP\": %lld, "
		"\"fpacketsIP\": %lld, "
		"\"icmpi\" : %lld, "
		"\"icmpo\" : %lld}",
		ss->net.tcp.InSegs,
		ss->net.tcp.OutSegs,
		ss->net.tcp.InErrs,
		ss->net.tcp.OutRsts,
		ss->net.tcp.ActiveOpens,
		ss->net.tcp.PassiveOpens,
		ss->net.tcp.RetransSegs,
		ss->net.udpv4.NoPorts,
		ss->net.udpv4.InErrors,
		ss->net.udpv4.InDatagrams +
		ss->net.udpv6.Udp6InDatagrams,
		ss->net.udpv4.OutDatagrams +
		ss->net.udpv6.Udp6OutDatagrams,
		ss->net.ipv4.InReceives  +
		ss->net.ipv6.Ip6InReceives,
		ss->net.ipv4.OutRequests +
		ss->net.ipv6.Ip6OutRequests,
		ss->net.ipv4.InDelivers +
		ss->net.ipv6.Ip6InDelivers,
		ss->net.ipv4.ForwDatagrams +
		ss->net.ipv6.Ip6OutForwDatagrams,
		ss->net.icmpv4.InMsgs +
		ss->net.icmpv6.Icmp6InMsgs,
		ss->net.icmpv4.OutMsgs +
		ss->net.icmpv6.Icmp6OutMsgs);
	output_samp(&defop, buf, buflen);

	char br[LEN_HP_SIZE];
        buflen = sprintf(br, ", %s: [", hp);
        output_samp(&defop, br, buflen);

	for (i = 0; ss->intf.intf[i].name[0]; i++) {
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"name\": \"%.19s\", "
			"\"rpack\": %lld, "
			"\"rbyte\": %lld, "
			"\"rerrs\": %lld, "
			"\"rdrops\": %lld, "
			"\"spack\": %lld, "
			"\"sbyte\": %lld, "
			"\"serrs\": %lld, "
			"\"sdrops\": %lld, "
			"\"speed\": \"%ld\", "
			"\"coll\": %lld, "
			"\"multi\": %lld, "
			"\"duplex\": %d}",
			ss->intf.intf[i].name,
			ss->intf.intf[i].rpack,
			ss->intf.intf[i].rbyte,
			ss->intf.intf[i].rerrs,
			ss->intf.intf[i].rdrop,
			ss->intf.intf[i].spack,
			ss->intf.intf[i].sbyte,
			ss->intf.intf[i].serrs,
			ss->intf.intf[i].sdrop,
			ss->intf.intf[i].speed,
			ss->intf.intf[i].scollis,
			ss->intf.intf[i].rmultic,
			ss->intf.intf[i].duplex);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_IFB(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

        char br[LEN_HP_SIZE];
        buflen = sprintf(br, ", %s: [", hp);
        output_samp(&defop, br, buflen);

	for (i = 0; i < ss->ifb.nrports; i++) {
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"ibname\": \"%.19s\", "
			"\"portnr\": \"%hd\", "
			"\"lanes\": \"%hd\", "
			"\"maxrate\": %lld, "
			"\"rcvb\": %lld, "
			"\"sndb\": %lld, "
			"\"rcvp\": %lld, "
			"\"sndp\": %lld}",
			ss->ifb.ifb[i].ibname,
			ss->ifb.ifb[i].portnr,
			ss->ifb.ifb[i].lanes,
			ss->ifb.ifb[i].rate,
			ss->ifb.ifb[i].rcvb,
			ss->ifb.ifb[i].sndb,
			ss->ifb.ifb[i].rcvp,
			ss->ifb.ifb[i].sndp);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_NUM(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

        char br[LEN_HP_SIZE];
        buflen = sprintf(br, ", %s: [", hp);
        output_samp(&defop, br, buflen);

	for (i = 0; i < ss->memnuma.nrnuma; i++) {
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"frag\": \"%f\", "
			"\"totmem\": %lld, "
			"\"freemem\": %lld, "
			"\"active\": %lld, "
			"\"inactive\": %lld, "
			"\"filepage\": %lld, "
			"\"dirtymem\": %lld, "
			"\"slabmem\": %lld, "
			"\"slabreclaim\": %lld, "
			"\"shmem\": %lld, "
			"\"tothp\": %lld}",
			ss->memnuma.numa[i].frag * 100.0,
			ss->memnuma.numa[i].totmem * pagesize,
			ss->memnuma.numa[i].freemem * pagesize,
			ss->memnuma.numa[i].active * pagesize,
			ss->memnuma.numa[i].inactive * pagesize,
			ss->memnuma.numa[i].filepage * pagesize,
			ss->memnuma.numa[i].dirtymem * pagesize,
			ss->memnuma.numa[i].slabmem * pagesize,
			ss->memnuma.numa[i].slabreclaim * pagesize,
			ss->memnuma.numa[i].shmem * pagesize,
			ss->memnuma.numa[i].tothp * ss->mem.hugepagesz);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_NUC(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

	char br[LEN_HP_SIZE];
	buflen = sprintf(br, ", %s: [", hp);
	output_samp(&defop, br, buflen);

	for (i = 0; i < ss->cpunuma.nrnuma; i++) {
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"numanr\": \"%d\", "
				"\"nrcpu\": %lld, "
				"\"stime\": %lld, "
				"\"utime\": %lld, "
				"\"ntime\": %lld, "
				"\"itime\": %lld, "
				"\"wtime\": %lld, "
				"\"Itime\": %lld, "
				"\"Stime\": %lld, "
				"\"steal\": %lld, "
				"\"guest\": %lld}",
				ss->cpunuma.numa[i].numanr,
				ss->cpunuma.numa[i].nrcpu,
				ss->cpunuma.numa[i].stime,
				ss->cpunuma.numa[i].utime,
				ss->cpunuma.numa[i].ntime,
				ss->cpunuma.numa[i].itime,
				ss->cpunuma.numa[i].wtime,
				ss->cpunuma.numa[i].Itime,
				ss->cpunuma.numa[i].Stime,
				ss->cpunuma.numa[i].steal,
				ss->cpunuma.numa[i].guest);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_LLC(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

        char br[LEN_HP_SIZE];
        buflen = sprintf(br, ", %s: [", hp);
        output_samp(&defop, br, buflen);

	for (i = 0; i < ss->llc.nrllcs; i++) {
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"LLC\": \"%3d\", "
			"\"occupancy\": \"%3.1f\", "
			"\"mbm_total\": \"%lld\", "
			"\"mbm_local\": %lld}",
			ss->llc.perllc[i].id,
			ss->llc.perllc[i].occupancy * 100,
			ss->llc.perllc[i].mbm_total,
			ss->llc.perllc[i].mbm_local);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

/*
** print functions for process-level statistics
*/
static void json_print_PRG(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i, exitcode;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];
	char br[LEN_HP_SIZE];
	buflen = sprintf(br, ", %s: [", hp);
	output_samp(&defop, br, buflen);

	static char st[3];

	for (i = 0; i < nact; i++, ps++) {
		/* For one thread whose pid==tgid and isproc=n, it has the same
		   value with pid==tgid and isproc=y, thus filter it out. */
		if (ps->gen.tgid == ps->gen.pid && !ps->gen.isproc)
			continue;

		if (ps->gen.excode & ~(INT_MAX))
			st[0]='N';
		else
			st[0]='-';

		if (ps->gen.excode & 0xff)      // killed by signal?
		{
			exitcode = (ps->gen.excode & 0x7f) + 256;
			if (ps->gen.excode & 0x80)
				st[1] = 'C';
			else
				st[1] = 'S';
		}
		else
		{
			exitcode = (ps->gen.excode >>   8) & 0xff;
			st[1] = 'E';
		}

		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}

		/* using getpwuid() & getpwuid to convert ruid & euid to string seems better, but the two functions take a long time */
		buflen = snprintf(buf, sizeof(buf), "{\"pid\": %d, "
			"\"name\": \"(%.19s)\", "
			"\"state\": \"%c\", "
			"\"ruid\": %d, "
			"\"rgid\": %d, "
			"\"tgid\": %d, "
			"\"nthr\": %d, "
			"\"st\": \"%s\", "
			"\"exitcode\": %d, "
			"\"btime\": \"%ld\", "
			"\"cmdline\": \"(%.130s)\", "
			"\"ppid\": %d, "
			"\"nthrrun\": %d, "
			"\"nthrslpi\": %d, "
			"\"nthrslpu\": %d, "
			"\"euid\": %d, "
			"\"egid\": %d, "
			"\"elaps\": \"%ld\", "
			"\"isproc\": %d, "
			"\"cid\": \"%.19s\"}",
			ps->gen.pid,
			ps->gen.name,
			ps->gen.state,
			ps->gen.ruid,
			ps->gen.rgid,
			ps->gen.tgid,
			ps->gen.nthr,
			st,
			exitcode,
			ps->gen.btime,
			ps->gen.cmdline,
			ps->gen.ppid,
			ps->gen.nthrrun,
			ps->gen.nthrslpi,
			ps->gen.nthrslpu,
			ps->gen.euid,
			ps->gen.egid,
			ps->gen.elaps,
			!!ps->gen.isproc, /* convert to boolean */
			ps->gen.container[0] ? ps->gen.container:"-");
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_PRC(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

        char br[LEN_HP_SIZE];
        buflen = sprintf(br, ", %s: [", hp);
        output_samp(&defop, br, buflen);

	for (i = 0; i < nact; i++, ps++) {
		if (ps->gen.tgid == ps->gen.pid && !ps->gen.isproc)
			continue;
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"pid\": %d, "
			"\"utime\": %lld, "
			"\"stime\": %lld, "
			"\"nice\": %d, "
			"\"prio\": %d, "
			"\"curcpu\": %d, "
			"\"tgid\": %d, "
			"\"isproc\": %d, "
			"\"rundelay\": %lld, "
			"\"blkdelay\": %lld, "
			"\"sleepavg\": %d}",
			ps->gen.pid,
			ps->cpu.utime,
			ps->cpu.stime,
			ps->cpu.nice,
			ps->cpu.prio,
			ps->cpu.curcpu,
			ps->gen.tgid,
			!!ps->gen.isproc,
			ps->cpu.rundelay/1000000,
			ps->cpu.blkdelay*1000/hertz,
			ps->cpu.sleepavg);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_PRM(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

        char br[LEN_HP_SIZE];
        buflen = sprintf(br, ", %s: [", hp);
        output_samp(&defop, br, buflen);

	for (i = 0; i < nact; i++, ps++) {
		if (ps->gen.tgid == ps->gen.pid && !ps->gen.isproc)
			continue;
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"pid\": %d, "
			"\"vmem\": %lld, "
			"\"rmem\": %lld, "
			"\"vexec\": %lld, "
			"\"vgrow\": %lld, "
			"\"rgrow\": %lld, "
			"\"minflt\": %lld, "
			"\"majflt\": %lld, "
			"\"vlibs\": %lld, "
			"\"vdata\": %lld, "
			"\"vstack\": %lld, "
			"\"vlock\": %lld, "
			"\"vswap\": %lld, "
			"\"pmem\": %lld}",
			ps->gen.pid,
			ps->mem.vmem,
			ps->mem.rmem,
			ps->mem.vexec,
			ps->mem.vgrow,
			ps->mem.rgrow,
			ps->mem.minflt,
			ps->mem.majflt,
			ps->mem.vlibs,
			ps->mem.vdata,
			ps->mem.vstack,
			ps->mem.vlock,
			ps->mem.vswap,
			ps->mem.pmem == (unsigned long long)-1LL ?
			0:ps->mem.pmem);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_PRD(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

        char br[LEN_HP_SIZE];
        buflen = sprintf(br, ", %s: [", hp);
        output_samp(&defop, br, buflen);

	for (i = 0; i < nact; i++, ps++) {
		if (ps->gen.tgid == ps->gen.pid && !ps->gen.isproc)
			continue;
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"pid\": %d, "
			"\"rio\": %lld, "
			"\"rsz\": %lld, "
			"\"wio\": %lld, "
			"\"wsz\": %lld, "
			"\"cwsz\": %lld}",
			ps->gen.pid,
			ps->dsk.rio, ps->dsk.rsz,
			ps->dsk.wio, ps->dsk.wsz, ps->dsk.cwsz);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_PRN(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	if (!(flags & NETATOP))
		return;

	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

        char br[LEN_HP_SIZE];
        buflen = sprintf(br, ", %s: [", hp);
        output_samp(&defop, br, buflen);

	for (i = 0; i < nact; i++, ps++) {
		if (ps->gen.tgid == ps->gen.pid && !ps->gen.isproc)
			continue;
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"pid\": %d, "
			"\"tcpsnd\": \"%lld\", "
			"\"tcpssz\": \"%lld\", "
			"\"tcprcv\": \"%lld\", "
			"\"tcprsz\": \"%lld\", "
			"\"udpsnd\": \"%lld\", "
			"\"udpssz\": \"%lld\", "
			"\"udprcv\": \"%lld\", "
			"\"udprsz\": \"%lld\"}",
			ps->gen.pid,
			ps->net.tcpsnd, ps->net.tcpssz,
			ps->net.tcprcv, ps->net.tcprsz,
			ps->net.udpsnd, ps->net.udpssz,
			ps->net.udprcv, ps->net.udprsz);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}

static void json_print_PRE(int flags, char *hp, struct sstat *ss, struct tstat *ps, int nact)
{
	if (!(flags & GPUSTAT) )
		return;

	int i;
	int buflen = 0;
	char buf[LINE_BUF_SIZE];

        char br[LEN_HP_SIZE];
        buflen = sprintf(br, ", %s: [", hp);
        output_samp(&defop, br, buflen);

	for (i = 0; i < nact; i++, ps++) {
		if (ps->gen.tgid == ps->gen.pid && !ps->gen.isproc)
			continue;
		if (i > 0) {
			output_samp(&defop, ", ", 2);
		}
		buflen = snprintf(buf, sizeof(buf), "{\"pid\": %d, "
			"\"gpustate\": \"%c\", "
			"\"nrgpus\": %d, "
			"\"gpulist\": \"%x\", "
			"\"gpubusy\": %d, "
			"\"membusy\": %d, "
			"\"memnow\": %lld, "
			"\"memcum\": %lld, "
			"\"sample\": %lld}",
			ps->gen.pid,
			ps->gpu.state == '\0' ? 'N':ps->gpu.state,
			ps->gpu.nrgpus,
			ps->gpu.gpulist,
			ps->gpu.gpubusy,
			ps->gpu.membusy,
			ps->gpu.memnow,
			ps->gpu.memcum,
			ps->gpu.sample);
		output_samp(&defop, buf, buflen);
	}

	output_samp(&defop, "]", 1);
}
