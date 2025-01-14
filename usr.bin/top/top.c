/*	$OpenBSD: top.c,v 1.107 2022/09/10 16:58:51 cheloha Exp $	*/

/*
 *  Top users/processes display for Unix
 *  Version 3
 *
 * Copyright (c) 1984, 1989, William LeFebvre, Rice University
 * Copyright (c) 1989, 1990, 1992, William LeFebvre, Northwestern University
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR OR HIS EMPLOYER BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <curses.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <poll.h>
#include <pwd.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <stdbool.h>

/* includes specific to top */
#include "display.h"		/* interface to display package */
#include "screen.h"		/* interface to screen package */
#include "top.h"
#include "top.local.h"
#include "machine.h"
#include "utils.h"

/* Size of the stdio buffer given to stdout */
#define BUFFERSIZE	2048

/* The buffer that stdio will use */
char		stdoutbuf[BUFFERSIZE];

/* signal handling routines */
static void	leave(int);
static void	onalrm(int);
static void	tstop(int);
static void	sigwinch(int);

volatile sig_atomic_t leaveflag, tstopflag, winchflag;

static void	reset_display(void);
int		rundisplay(void);

static int	max_topn;	/* maximum displayable processes */
static int	skip;		/* how many processes to skip (scroll) */

extern int ncpu;
extern int ncpuonline;

extern int	(*proc_compares[])(const void *, const void *);
int order_index;
int rev_order;

int displays = 0;	/* indicates unspecified */
int do_unames = true;
struct process_select ps;
int interactive = -1;	/* indicates undefined */
double delay = Default_DELAY;
char *order_name = NULL;
int topn = Default_TOPN;
int no_command = true;
int old_system = false;
int old_threads = false;
int show_args = false;
pid_t hlpid = (pid_t)-1;
int combine_cpus = 0;

#if Default_TOPN == Infinity
int topn_specified = false;
#endif

struct system_info system_info;
struct statics  statics;

/*
 * these defines enumerate the "strchr"s of the commands in
 * command_chars
 */
#define CMD_redraw	0
#define CMD_update	1
#define CMD_quit	2
#define CMD_help1	3
#define CMD_help2	4
#define CMD_OSLIMIT	4	/* terminals with OS can only handle commands */
#define CMD_errors	5	/* less than or equal to CMD_OSLIMIT	   */
#define CMD_number1	6
#define CMD_number2	7
#define CMD_delay	8
#define CMD_displays	9
#define CMD_kill	10
#define CMD_renice	11
#define CMD_idletog	12
#define CMD_idletog2	13
#define CMD_user	14
#define CMD_system	15
#define CMD_order	16
#define CMD_pid		17
#define CMD_command	18
#define CMD_threads	19
#define CMD_grep	20
#define CMD_add		21
#define CMD_hl		22
#define CMD_cpus	23
#define CMD_down	24
#define CMD_up		25
#define CMD_pagedown	26
#define CMD_pageup	27
#define CMD_grep2	28
#define CMD_rtableid	29
#define CMD_rtable	30

static void
usage(void)
{
	extern char *__progname;

	fprintf(stderr,
	    "usage: %s [-1bCHIinqStu] [-d count] [-g string] [-o [-]field] "
	    "[-p pid] [-s time]\n\t[-T [-]rtable] [-U [-]user] [number]\n",
	    __progname);
}

static int
getorder(char *field)
{
	int i, r = field[0] == '-';

	i = string_index(r ? field + 1 : field, statics.order_names);
	if (i != -1)
		rev_order = r;

	return i;
}

static int
filteruser(char buf[])
{
	const char *errstr;
	char *bufp = buf;
	uid_t *uidp;
	uid_t uid;

	if (bufp[0] == '-') {
		bufp++;
		uidp = &ps.huid;
		ps.uid = (pid_t)-1;
	} else {
		uidp = &ps.uid;
		ps.huid = (pid_t)-1;
	}

	if (uid_from_user(bufp, uidp) == 0)
		return 0;

	uid = strtonum(bufp, 0, UID_MAX, &errstr);
	if (errstr == NULL && user_from_uid(uid, 1) != NULL) {
		*uidp = uid;
		return 0;
	}

	return -1;
}

static int
filterpid(char buf[], int hl)
{
	const char *errstr;
	int pid;

	pid = strtonum(buf, 0, INT_MAX, &errstr);
	if (errstr != NULL || !find_pid(pid))
		return -1;

	if (hl)
		hlpid = (pid_t)pid;
	else {
		if (!ps.system)
			old_system = false;
		ps.pid = (pid_t)pid;
		ps.system = true;
	}

	return 0;
}

static int
filterrtable(char buf[])
{
	const char *errstr;
	char *bufp = buf;
	uint32_t *rtableidp;
	uint32_t rtableid;

	if (bufp[0] == '-') {
		bufp++;
		rtableidp = &ps.hrtableid;
		ps.rtableid = -1;
	} else {
		rtableidp = &ps.rtableid;
		ps.hrtableid = -1;
	}

	rtableid = strtonum(bufp, 0, RT_TABLEID_MAX, &errstr);
	if (errstr == NULL) {
		*rtableidp = rtableid;
		return 0;
	}

	return -1;
}

static void
parseargs(int ac, char **av)
{
	char *endp;
	int i;

	while ((i = getopt(ac, av, "1SHICbinqus:d:p:U:o:g:T:t")) != -1) {
		switch (i) {
		case '1':
			combine_cpus = 1;
			break;
		case 'C':
			show_args = true;
			break;
		case 'u':	/* toggle uid/username display */
			do_unames = !do_unames;
			break;

		case 'U':	/* display only username's processes */
			if (filteruser(optarg) == -1)
				new_message(MT_delayed, "%s: unknown user",
				    optarg);
			break;

		case 'p':	/* display only process id */
			if (filterpid(optarg, false) == -1)
				new_message(MT_delayed, "%s: unknown pid",
				    optarg);
			break;

		case 'S':	/* show system processes */
			ps.system = !ps.system;
			old_system = !old_system;
			break;

		case 'H':	/* show threads */
			ps.threads = true;
			old_threads = true;
			break;

		case 'I':	/* show idle processes */
			ps.idle = !ps.idle;
			break;

		case 'i':	/* go interactive regardless */
			interactive = true;
			break;

		case 'n':	/* batch, or non-interactive */
		case 'b':
			interactive = false;
			break;

		case 'd':	/* number of displays to show */
			if ((i = atoiwi(optarg)) != Invalid && i != 0) {
				displays = i;
				if (displays == 1)
					interactive = false;
				break;
			}
			new_message(MT_delayed,
			    "warning: display count should be positive "
			    "-- option ignored");
			break;

		case 's':
			delay = strtod(optarg, &endp);

			if (delay >= 0 && delay <= 1000000 && *endp == '\0')
				break;

			new_message(MT_delayed,
			    "warning: delay should be a non-negative number"
			    " -- using default");
			delay = Default_DELAY;
			break;

		case 'q':	/* be quick about it */
			/* only allow this if user is really root */
			if (getuid() == 0) {
				/* be very un-nice! */
				(void) nice(-20);
				break;
			}
			new_message(MT_delayed,
			    "warning: `-q' option can only be used by root");
			break;

		case 'o':	/* select sort order */
			order_name = optarg;
			break;

		case 'g':	/* grep command name */
			free(ps.command);
			if ((ps.command = strdup(optarg)) == NULL)
				err(1, NULL);
			break;

		case 'T':
			if (filterrtable(optarg) == -1)
				new_message(MT_delayed,
				    "%s: invalid routing table", optarg);
			break;

		case 't':
			ps.rtable = true;
			break;

		default:
			usage();
			exit(1);
		}
	}

	i = getncpuonline();
	if (i == -1)
		err(1, NULL);

	if (i > 8)
		combine_cpus = 1;

	/* get count of top processes to display (if any) */
	if (optind < ac) {
		if ((topn = atoiwi(av[optind])) == Invalid) {
			new_message(MT_delayed,
			    "warning: process count should "
			    "be a non-negative number -- using default");
			topn = Infinity;
		}
#if Default_TOPN == Infinity
		else
			topn_specified = true;
#endif
	}
}

int
main(int argc, char *argv[])
{
	char *header_text, *env_top;
	char *uname_field = "USERNAME", *thread_field = "     TID";
	char *wait_field = "WAIT   ", *rtable_field = " RTABLE";
	const char *(*get_userid)(uid_t, int) = user_from_uid;
	char **preset_argv = NULL, **av = argv;
	int preset_argc = 0, ac = argc, active_procs, i, ncpuonline_now;
	sigset_t mask, oldmask;
	time_t curr_time;
	struct handle *processes;

	/* set the buffer for stdout */
#ifdef DEBUG
	setvbuf(stdout, NULL, _IONBUF, 0);
#else
	setvbuf(stdout, stdoutbuf, _IOFBF, sizeof stdoutbuf);
#endif

	/* initialize some selection options */
	ps.idle = true;
	ps.system = false;
	ps.uid = (uid_t)-1;
	ps.huid = (uid_t)-1;
	ps.pid = (pid_t)-1;
	ps.rtable = false;
	ps.rtableid = -1;
	ps.hrtableid = -1;
	ps.command = NULL;

	/* get preset options from the environment */
	if ((env_top = getenv("TOP")) != NULL) {
		av = preset_argv = argparse(env_top, &preset_argc);
		ac = preset_argc;

		/*
		 * set the dummy argument to an explanatory message, in case
		 * getopt encounters a bad argument
		 */
		preset_argv[0] = "while processing environment";
	}
	/* process options */
	do {
		/*
		 * if we're done doing the presets, then process the real
		 * arguments
		 */
		if (preset_argc == 0) {
			ac = argc;
			av = argv;
			optind = 1;
		}
		parseargs(ac, av);
		i = preset_argc;
		preset_argc = 0;
	} while (i != 0);

	if (pledge("stdio rpath getpw tty proc ps vminfo", NULL) == -1)
		err(1, "pledge");

	/* set constants for username/uid display correctly */
	if (!do_unames) {
		uname_field = "   UID  ";
		get_userid = format_uid;
	}
	/* initialize the kernel memory interface */
	if (machine_init(&statics) == -1)
		exit(1);

	/* determine sorting order index, if necessary */
	if (order_name != NULL) {
		if ((order_index = getorder(order_name)) == -1) {
			new_message(MT_delayed,
			    " %s: unrecognized sorting order", order_name);
			order_index = 0;
		}
	}

	/* initialize termcap */
	init_termcap(interactive);

	/* initialize display interface */
	max_topn = display_init(&statics);

	/* print warning if user requested more processes than we can display */
	if (topn > max_topn)
		new_message(MT_delayed,
		    "warning: this terminal can only display %d processes",
		    max_topn);
	/* adjust for topn == Infinity */
	if (topn == Infinity) {
		/*
		 *  For smart terminals, infinity really means everything that can
		 *  be displayed, or Largest.
		 *  On dumb terminals, infinity means every process in the system!
		 *  We only really want to do that if it was explicitly specified.
		 *  This is always the case when "Default_TOPN != Infinity".  But if
		 *  topn wasn't explicitly specified and we are on a dumb terminal
		 *  and the default is Infinity, then (and only then) we use
		 *  "Nominal_TOPN" instead.
		 */
#if Default_TOPN == Infinity
		topn = smart_terminal ? Largest :
		    (topn_specified ? Largest : Nominal_TOPN);
#else
		topn = Largest;
#endif
	}
	/* set header display accordingly */
	display_header(topn > 0);

	/* determine interactive state */
	if (interactive == -1)
		interactive = smart_terminal;

	/* if # of displays not specified, fill it in */
	if (displays == 0)
		displays = smart_terminal ? Infinity : 1;

	/*
	 * block interrupt signals while setting up the screen and the
	 * handlers
	 */
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGQUIT);
	sigaddset(&mask, SIGTSTP);
	sigprocmask(SIG_BLOCK, &mask, &oldmask);
	if (interactive)
		init_screen();
	if (pledge("stdio getpw tty proc ps vminfo", NULL) == -1)
		err(1, "pledge");
	(void) signal(SIGINT, leave);
	siginterrupt(SIGINT, 1);
	(void) signal(SIGQUIT, leave);
	(void) signal(SIGTSTP, tstop);
	if (smart_terminal)
		(void) signal(SIGWINCH, sigwinch);
	sigprocmask(SIG_SETMASK, &oldmask, NULL);
restart:

	/*
	 *  main loop -- repeat while display count is positive or while it
	 *		indicates infinity (by being -1)
	 */
	while ((displays == -1) || (displays-- > 0)) {
		if (winchflag) {
			/*
			 * reascertain the screen
			 * dimensions
			 */
			get_screensize();
			resizeterm(screen_length, screen_width + 1);

			/* tell display to resize */
			max_topn = display_resize();

			/* reset the signal handler */
			(void) signal(SIGWINCH, sigwinch);

			reset_display();
			winchflag = 0;
		}

		/* get the current stats */
		get_system_info(&system_info);

		/*
		 * don't display stats for offline CPUs: resize if we're
		 * interactive and CPUs have toggled on or offline
		 */
		if (interactive && !combine_cpus) {
			for (i = ncpuonline_now = 0; i < ncpu; i++)
				if (system_info.cpuonline[i])
					ncpuonline_now++;
			if (ncpuonline_now != ncpuonline) {
				max_topn = display_resize();
				reset_display();
				continue;
			}
		}

		/* get the current set of processes */
		processes = get_process_info(&system_info, &ps,
		    proc_compares[order_index]);

		/* display the load averages */
		i_loadave(system_info.load_avg);

		/* display the current time */
		/* this method of getting the time SHOULD be fairly portable */
		time(&curr_time);
		i_timeofday(&curr_time);

		/* display process/threads state breakdown */
		i_procstates(system_info.p_total, system_info.procstates,
		    ps.threads);

		/* display the cpu state percentage breakdown */
		i_cpustates(system_info.cpustates, system_info.cpuonline);

		/* display memory stats */
		i_memory(system_info.memory);

		/* handle message area */
		i_message();

		/* get the string to use for the process area header */
		header_text = format_header(
		    ps.threads ? thread_field : uname_field,
		    ps.rtable ? rtable_field : wait_field);

		/* update the header area */
		i_header(header_text);

		if (topn == Infinity) {
#if Default_TOPN == Infinity
			topn = smart_terminal ? Largest :
			    (topn_specified ? Largest : Nominal_TOPN);
#else
			topn = Largest;
#endif
		}

		if (topn > 0) {
			/* determine number of processes to actually display */
			/*
			 * this number will be the smallest of:  active
			 * processes, number user requested, number current
			 * screen accommodates
			 */
			active_procs = system_info.p_active;
			if (active_procs > topn)
				active_procs = topn;
			if (active_procs > max_topn)
				active_procs = max_topn;
			/* determine how many process to skip, if asked to */
			/*
			 * this number is tweaked by user, but gets shrinked
			 * when number of active processes lowers too much
			 */
			if (skip + active_procs > system_info.p_active)
				skip = system_info.p_active - active_procs;
			skip_processes(processes, skip);
			/* now show the top "n" processes. */
			for (i = 0; i < active_procs; i++) {
				pid_t pid;
				char * s;

				s = format_next_process(processes,
				    ps.threads ? NULL : get_userid, ps.rtable,
				    &pid);
				i_process(i, s, pid == hlpid);
			}
		}

		/* do end-screen processing */
		u_endscreen();

		/* now, flush the output buffer */
		fflush(stdout);

		if (smart_terminal)
			refresh();

		/* only do the rest if we have more displays to show */
		if (displays) {
			/* switch out for new display on smart terminals */
			no_command = true;
			if (!interactive) {
				/* set up alarm */
				(void) signal(SIGALRM, onalrm);
				(void) alarm((unsigned) delay);

				/* wait for the rest of it .... */
				pause();
				if (leaveflag)
					exit(0);
				if (tstopflag) {
					(void) signal(SIGTSTP, SIG_DFL);
					(void) kill(0, SIGTSTP);
					/* reset the signal handler */
					(void) signal(SIGTSTP, tstop);
					tstopflag = 0;
				}
			} else {
				while (no_command)
					if (rundisplay())
						goto restart;
			}
		}
	}

	quit(0);
	/* NOTREACHED */
	return (0);
}

int
rundisplay(void)
{
	static char tempbuf[TEMPBUFSIZE];
	sigset_t mask;
	char ch, *iptr;
	int change, i;
	struct pollfd pfd[1];
	static char command_chars[] = "\f qh?en#sdkriIuSopCHg+P109)(/Tt";

	/*
	 * assume valid command unless told
	 * otherwise
	 */
	no_command = false;

	/*
	 * set up arguments for select with
	 * timeout
	 */
	pfd[0].fd = STDIN_FILENO;
	pfd[0].events = POLLIN;

	if (leaveflag)
		quit(0);
	if (tstopflag) {
		/* move to the lower left */
		end_screen();
		fflush(stdout);

		/*
		 * default the signal handler
		 * action
		 */
		(void) signal(SIGTSTP, SIG_DFL);

		/*
		 * unblock the signal and
		 * send ourselves one
		 */
		sigemptyset(&mask);
		sigaddset(&mask, SIGTSTP);
		sigprocmask(SIG_UNBLOCK, &mask, NULL);
		(void) kill(0, SIGTSTP);

		/* reset the signal handler */
		(void) signal(SIGTSTP, tstop);

		/* reinit screen */
		reinit_screen();
		reset_display();
		tstopflag = 0;
		return 1;
	}
	/*
	 * wait for either input or the end
	 * of the delay period
	 */
	if (poll(pfd, 1, (int)(delay * 1000)) > 0) {
		char *errmsg;
		ssize_t len;

		if ((pfd[0].revents & (POLLERR|POLLHUP|POLLNVAL)))
			exit(1);

		clear_message();

		/*
		 * now read it and convert to
		 * command strchr
		 */
		while (1) {
			len = read(STDIN_FILENO, &ch, 1);
			if (len == -1 && errno == EINTR)
				continue;
			if (len == 0)
				exit(1);
			break;
		}
		if ((iptr = strchr(command_chars, ch)) == NULL) {
			/* illegal command */
			new_message(MT_standout, " Command not understood");
			putr();
			no_command = true;
			fflush(stdout);
			return (0);
		}

		change = iptr - command_chars;

		switch (change) {
		case CMD_redraw:	/* redraw screen */
			reset_display();
			break;

		case CMD_update:	/* merely update display */
			/*
			 * is the load average high?
			 */
			if (system_info.load_avg[0] > LoadMax) {
				/* yes, go home for visual feedback */
				go_home();
				fflush(stdout);
			}
			break;

		case CMD_quit:	/* quit */
			quit(0);
			break;

		case CMD_help1:	/* help */
		case CMD_help2:
			clear();
			show_help();
			anykey();
			clear();
			break;

		case CMD_errors:	/* show errors */
			if (error_count() == 0) {
				new_message(MT_standout,
				    " Currently no errors to report.");
				putr();
				no_command = true;
			} else {
				clear();
				show_errors();
				anykey();
				clear();
			}
			break;

		case CMD_number1:	/* new number */
		case CMD_number2:
			new_message(MT_standout,
			    "Number of processes to show: ");

			if (readline(tempbuf, 8) > 0) {
				if ((i = atoiwi(tempbuf)) != Invalid) {
					if (i > max_topn) {
						new_message(MT_standout |
						    MT_delayed,
						    " This terminal can only "
						    "display %d processes.",
						    max_topn);
						putr();
					}
					if ((i > topn || i == Infinity)
					    && topn == 0) {
						/* redraw the header */
						display_header(true);
					} else if (i == 0)
						display_header(false);
					topn = i;
				} else {
					new_message(MT_standout,
					    "Processes should be a "
					    "non-negative number");
					putr();
					no_command = true;
				}
			} else
				clear_message();
			break;

		case CMD_delay:	/* new seconds delay */
			new_message(MT_standout, "Seconds to delay: ");
			if (readline(tempbuf, sizeof(tempbuf)) > 0) {
				char *endp;
				double newdelay = strtod(tempbuf, &endp);

				if (newdelay >= 0 && newdelay <= 1000000 &&
				    *endp == '\0') {
					delay = newdelay;
				} else {
					new_message(MT_standout,
					    "Delay should be a non-negative number");
					putr();
					no_command = true;
				}

			} else
				clear_message();
			break;

		case CMD_displays:	/* change display count */
			new_message(MT_standout,
			    "Displays to show (currently %s): ",
			    displays == -1 ? "infinite" :
			    itoa(displays));

			if (readline(tempbuf, 10) > 0) {
				if ((i = atoiwi(tempbuf)) != Invalid) {
					if (i == 0)
						quit(0);
					displays = i;
				} else {
					new_message(MT_standout,
					    "Displays should be a non-negative number");
					putr();
					no_command = true;
				}
			} else
				clear_message();
			break;

		case CMD_kill:	/* kill program */
			new_message(0, "kill ");
			if (readline(tempbuf, sizeof(tempbuf)) > 0) {
				if ((errmsg = kill_procs(tempbuf)) != NULL) {
					new_message(MT_standout, "%s", errmsg);
					putr();
					no_command = true;
				}
			} else
				clear_message();
			break;

		case CMD_renice:	/* renice program */
			new_message(0, "renice ");
			if (readline(tempbuf, sizeof(tempbuf)) > 0) {
				if ((errmsg = renice_procs(tempbuf)) != NULL) {
					new_message(MT_standout, "%s", errmsg);
					putr();
					no_command = true;
				}
			} else
				clear_message();
			break;

		case CMD_idletog:
		case CMD_idletog2:
			ps.idle = !ps.idle;
			new_message(MT_standout | MT_delayed,
			    " %sisplaying idle processes.",
			    ps.idle ? "D" : "Not d");
			putr();
			break;

		case CMD_user:
			new_message(MT_standout,
			    "Username to show: ");
			if (readline(tempbuf, sizeof(tempbuf)) > 0) {
				if ((tempbuf[0] == '+' || tempbuf[0] == '-') &&
				    tempbuf[1] == '\0') {
					ps.uid = (uid_t)-1;
					ps.huid = (uid_t)-1;
				} else if (filteruser(tempbuf) == -1) {
					new_message(MT_standout,
					    " %s: unknown user",
					    tempbuf[0] == '-' ? tempbuf + 1 : 
					    tempbuf);
					no_command = true;
				}
				putr();
			} else
				clear_message();
			break;

		case CMD_system:
			ps.system = !ps.system;
			old_system = ps.system;
			new_message(MT_standout | MT_delayed,
			    " %sisplaying system processes.",
			    ps.system ? "D" : "Not d");
			break;

		case CMD_order:
			new_message(MT_standout,
			    "Order to sort: ");
			if (readline(tempbuf, sizeof(tempbuf)) > 0) {
				if ((i = getorder(tempbuf)) == -1) {
					new_message(MT_standout,
					    " %s: unrecognized sorting order",
					    tempbuf[0] == '-' ? tempbuf + 1 :
					    tempbuf);
					no_command = true;
				} else
					order_index = i;
				putr();
			} else
				clear_message();
			break;

		case CMD_pid:
			new_message(MT_standout, "Process ID to show: ");
			if (readline(tempbuf, sizeof(tempbuf)) > 0) {
				if (tempbuf[0] == '+' &&
				    tempbuf[1] == '\0') {
					ps.pid = (pid_t)-1;
					ps.system = old_system;
				} else if (filterpid(tempbuf, 0) == -1) {
					new_message(MT_standout,
					    " %s: unknown pid", tempbuf);
					no_command = true;
				}
				putr();
			} else
				clear_message();
			break;

		case CMD_command:
			show_args = !show_args;
			break;

		case CMD_threads:
			ps.threads = !ps.threads;
			old_threads = ps.threads;
			new_message(MT_standout | MT_delayed,
			    " %sisplaying threads.",
			    ps.threads ? "D" : "Not d");
			break;

		case CMD_grep:
		case CMD_grep2:
			new_message(MT_standout,
			    "Grep command name: ");
			if (readline(tempbuf, sizeof(tempbuf)) > 0) {
				free(ps.command);
				if (tempbuf[0] == '+' &&
				    tempbuf[1] == '\0')
					ps.command = NULL;
				else if ((ps.command = strdup(tempbuf)) == NULL)
					err(1, NULL);
				putr();
			} else
				clear_message();
			break;

		case CMD_hl:
			new_message(MT_standout, "Process ID to highlight: ");
			if (readline(tempbuf, sizeof(tempbuf)) > 0) {
				if (tempbuf[0] == '+' &&
				    tempbuf[1] == '\0') {
					hlpid = (pid_t)-1;
				} else if (filterpid(tempbuf, true) == -1) {
					new_message(MT_standout,
					    " %s: unknown pid", tempbuf);
					no_command = true;
				}
				putr();
			} else
				clear_message();
			break;

		case CMD_add:
			ps.uid = (uid_t)-1;	/* uid */
			ps.huid = (uid_t)-1;
			ps.pid = (pid_t)-1;	/* pid */
			ps.rtableid = -1;	/* rtableid */
			ps.hrtableid = -1;
			ps.system = old_system;
			ps.command = NULL;	/* grep */
			hlpid = (pid_t)-1;
			break;
		case CMD_cpus:
			combine_cpus = !combine_cpus;
			max_topn = display_resize();
			reset_display();
			break;
		case CMD_down:
			skip++;
			break;
		case CMD_up:
			if (skip > 0)
				skip--;
			break;
		case CMD_pagedown:
			skip += max_topn / 2;
			break;
		case CMD_pageup:
			skip -= max_topn / 2;
			if (skip < 0)
				skip = 0;
			break;
		case CMD_rtableid:
			new_message(MT_standout,
			    "Routing table: ");
			if (readline(tempbuf, sizeof(tempbuf)) > 0) {
				if (tempbuf[0] == '+' && tempbuf[1] == '\0') {
					ps.rtableid = -1;
					ps.hrtableid = -1;
				} else if (filterrtable(tempbuf) == -1) {
					new_message(MT_standout,
					    " %s: invalid routing table",
					    tempbuf[0] == '-' ? tempbuf + 1 :
					    tempbuf);
					no_command = true;
				}
				putr();
			} else
				clear_message();
			break;
		case CMD_rtable:
			ps.rtable = !ps.rtable;
			new_message(MT_standout | MT_delayed,
			    " %sisplaying routing tables.",
			    ps.rtable ? "D" : "Not d");
			break;
		default:
			new_message(MT_standout, " BAD CASE IN SWITCH!");
			putr();
		}
	}

	/* flush out stuff that may have been written */
	fflush(stdout);
	return 0;
}


/*
 *  reset_display() - reset all the display routine pointers so that entire
 *	screen will get redrawn.
 */
static void
reset_display(void)
{
	if (smart_terminal) {
		clear();
		refresh();
	}
}

/* ARGSUSED */
void
leave(int signo)
{
	leaveflag = 1;
}

/* ARGSUSED */
void
tstop(int signo)
{
	tstopflag = 1;
}

/* ARGSUSED */
void
sigwinch(int signo)
{
	winchflag = 1;
}

/* ARGSUSED */
void
onalrm(int signo)
{
}

void
quit(int ret)
{
	end_screen();
	exit(ret);
}
