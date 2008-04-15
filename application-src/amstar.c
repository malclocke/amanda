/*
 * Amanda, The Advanced Maryland Automatic Network Disk Archiver
 * Copyright (c) 1991-1998 University of Maryland at College Park
 * All Rights Reserved.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of U.M. not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  U.M. makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * U.M. DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL U.M.
 * BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors: the Amanda Development Team.  Its members are listed in a
 * file named AUTHORS, in the root directory of this distribution.
 */
/* 
 * $Id: amstar.c 8888 2007-10-02 13:40:42Z martineau $
 *
 * send estimated backup sizes using dump
 */

/* PROPERTY:
 *
 * STAR-PATH (default STAR)
 * STAR-TARDUMP
 * STAR-DLE-TARDUMP
 * ONE-FILE-SYSTEM
 * SPARSE
 */

#include "amanda.h"
#include "pipespawn.h"
#include "amfeatures.h"
#include "amandates.h"
#include "clock.h"
#include "util.h"
#include "getfsent.h"
#include "version.h"
#include "client_util.h"
#include "conffile.h"
#include "amandad.h"
#include "getopt.h"
#include "sendbackup.h"

int debug_application = 1;
#define application_debug(i, ...) do {	\
	if ((i) <= debug_application) {	\
	    dbprintf(__VA_ARGS__);	\
	}				\
} while (0)

static amregex_t re_table[] = {
  /* tar prints the size in bytes */
  AM_SIZE_RE("star: [0-9][0-9]* blocks", 10240, 1),
  AM_NORMAL_RE("^could not open conf file"),
  AM_NORMAL_RE("^Type of this level "),
  AM_NORMAL_RE("^Date of this level "),
  AM_NORMAL_RE("^Date of last level "),
  AM_NORMAL_RE("^Dump record  level "),
  AM_NORMAL_RE("^Throughput"),
  AM_NORMAL_RE("^.*is sparse$"),

#ifdef IGNORE_TAR_ERRORS
  AM_NORMAL_RE("^.*shrunk*$"),
  AM_NORMAL_RE("^.*changed size.*$"),
  AM_NORMAL_RE("^.*Cannot listxattr for.*$"),
  AM_NORMAL_RE("^.Cannot: stat .*$"),
  AM_NORMAL_RE("^.Missing links .*$"),
  AM_NORMAL_RE("^.Cannot get xattr.*$"),
  AM_NORMAL_RE("^.Cannot.*acl.*$"),
#endif

  AM_NORMAL_RE("^star: dumped [0-9][0-9]* (tar )?files"),
  AM_NORMAL_RE("^.*The following problems occurred during .* processing.*$"),
  AM_NORMAL_RE("^.*Processed all possible files, despite earlier errors.*$"),
  AM_NORMAL_RE("^.*not written due to problems during backup.*$"),

  AM_STRANGE_RE("^Perform a level 0 dump first.*$"),

  /* catch-all: DMP_STRANGE is returned for all other lines */
  AM_STRANGE_RE(NULL)
};

/* local functions */
int main(int argc, char **argv);

typedef struct application_argument_s {
    char      *config;
    char      *host;
    int        message;
    int        collection;
    int        level;
    dle_t      dle;
    int        argc;
    char     **argv;
    proplist_t property;
} application_argument_t;

enum { CMD_ESTIMATE, CMD_BACKUP };

static void amstar_support(application_argument_t *argument);
static void amstar_selfcheck(application_argument_t *argument);
static void amstar_estimate(application_argument_t *argument);
static void amstar_backup(application_argument_t *argument);
static void amstar_restore(application_argument_t *argument);
static char **amstar_build_argv(application_argument_t *argument,
				int command);
char *star_path;
char *star_tardumps;
int   star_dle_tardumps;
int   star_onefilesystem;
int   star_sparse;

static struct option long_options[] = {
    {"config"    , 1, NULL, 1},
    {"host"      , 1, NULL, 2},
    {"disk"      , 1, NULL, 3},
    {"device"    , 1, NULL, 4},
    {"level"     , 1, NULL, 5},
    {"index"     , 1, NULL, 6},
    {"message"   , 1, NULL, 7},
    {"collection", 0, NULL, 8},
    {"record"    , 0, NULL, 9}
};


int
main(
    int		argc,
    char **	argv)
{
    char *line = NULL;
    int c;
    char *command;
    application_argument_t argument;
    char *s, *fp, ch;
    char *prop_name;
    char *prop_value;
    GSList *property_values;
    GSList *prop_values;

#ifdef STAR
    star_path = STAR;
#else
    star_path = NULL;
#endif
    star_tardumps = "/etc/tardumps";
    star_dle_tardumps = 0;
    star_onefilesystem = 1;
    star_sparse = 1;

    /* initialize */

    /*
     * Configure program for internationalization:
     *   1) Only set the message locale for now.
     *   2) Set textdomain for all amanda related programs to "amanda"
     *      We don't want to be forced to support dozens of message catalogs.
     */  
    setlocale(LC_MESSAGES, "C");
    textdomain("amanda"); 

    /* drop root privileges */

    if (!set_root_privs(0)) {
	error(_("amstar must be run setuid root"));
    }

    safe_fd(3, 1);

    set_pname("amstar");

    /* Don't die when child closes pipe */
    signal(SIGPIPE, SIG_IGN);

#if defined(USE_DBMALLOC)
    malloc_size_1 = malloc_inuse(&malloc_hist_1);
#endif

    erroutput_type = (ERR_INTERACTIVE|ERR_SYSLOG);
    dbopen(DBG_SUBDIR_CLIENT);
    startclock();
    dbprintf(_("version %s\n"), version());

    config_init(CONFIG_INIT_CLIENT, NULL);

    //check_running_as(RUNNING_AS_DUMPUSER_PREFERRED);
    //root for amrecover
    //RUNNING_AS_CLIENT_LOGIN from selfcheck, sendsize, sendbackup

    /* parse argument */
    command = argv[1];

    argument.config     = NULL;
    argument.host       = NULL;
    argument.message    = 0;
    argument.collection = 0;
    argument.level      = 0;
    argument.property   = NULL;
    init_dle(&argument.dle);

    while (1) {
	int option_index = 0;
    	c = getopt_long (argc-1, argv+1, "", long_options, &option_index);
	if (c == -1)
	    break;

	switch (c) {
	case 1: argument.config = stralloc(optarg);
		break;
	case 2: argument.host = stralloc(optarg);
		break;
	case 3: argument.dle.disk = stralloc(optarg);
		break;
	case 4: argument.dle.device = stralloc(optarg);
		break;
	case 5: argument.level = atoi(optarg);
		break;
	case 6: argument.dle.create_index = 1;
		break;
	case 7: argument.message = 1;
		break;
	case 8: argument.collection = 1;
		break;
	case 9: argument.dle.record = 1;
		break;
	case ':':
	case '?':
		break;
	}
    }

    if (argument.config) {
	/* overlay this configuration on the existing (nameless) configuration */
	config_init(CONFIG_INIT_CLIENT | CONFIG_INIT_EXPLICIT_NAME | CONFIG_INIT_OVERLAY,
		    argument.config);
	dbrename(get_config_name(), DBG_SUBDIR_CLIENT);

    }

    if (config_errors(NULL) >= CFGERR_ERRORS) {
	g_critical(_("errors processing config file"));
    }

    argument.argc = argc - optind;
    argument.argv = argv + optind;

    /* parse property */
    argument.property = g_hash_table_new_full(g_str_hash, g_str_equal,
					      NULL, NULL);
    for(; (line = agets(stdin)) != NULL; free(line)) {
	s = line;
	ch = *s++;
	skip_whitespace(s, ch);
	if (ch == '\0') {
	    error(_("No property name\n"));
	}
	fp = s - 1;
	skip_quoted_string(s, ch);
	s[-1] = '\0';
	prop_name = unquote_string(fp);
	skip_whitespace(s, ch);
	if (ch == '\0') {
	    error(_("No property value\n"));
	}
	property_values = NULL;
	property_values = g_hash_table_lookup(argument.property, prop_name);
	while (ch != '\0') {
	    fp = s - 1;
	    skip_quoted_string(s, ch);
	    s[-1] = '\0';
	    prop_value = unquote_string(fp);
	    property_values = g_slist_append(property_values, prop_value);
	    skip_whitespace(s, ch);
	}
	if (strcmp(command, "selfcheck") == 0 &&
	    (strcmp(prop_name, "STAR-PATH")         != 0 &&
	     strcmp(prop_name, "STAR-TARDUMPS")     != 0 &&
	     strcmp(prop_name, "STAR-DLE-TARDUMPS") != 0 &&
	     strcmp(prop_name, "ONE-FILE-SYSTEM")   != 0 &&
	     strcmp(prop_name, "SPARSE")            != 0)) {
	    printf(_("ERROR [amstar: Unknown property '%s']\n"), prop_name);
	}

	g_hash_table_insert(argument.property, prop_name, property_values);
    }
    fclose(stdin);

    if ((prop_values = g_hash_table_lookup(argument.property,
					   "STAR-PATH"))) {
	star_path = (char *)prop_values->data;
    }
    if ((prop_values = g_hash_table_lookup(argument.property,
					   "STAR-TARDUMPS"))) {
	star_tardumps = (char *)prop_values->data;
    }

    if ((prop_values = g_hash_table_lookup(argument.property,
					   "STAR-DLE-TARDUMPS"))) {
	if (strcasecmp((char *)prop_values->data, "yes") == 0)
	    star_dle_tardumps = 1;
    }

    if ((prop_values = g_hash_table_lookup(argument.property,
					   "ONE-FILE-SYSTEM"))) {
	if (strcasecmp((char *)prop_values->data, "yes") != 0)
	    star_onefilesystem = 0;
    }

    if ((prop_values = g_hash_table_lookup(argument.property,
					   "SPARSE"))) {
	if (strcasecmp((char *)prop_values->data, "yes") != 0)
	    star_sparse = 0;
    }

    if (strcmp(command, "support") == 0) {
	amstar_support(&argument);
    } else if (strcmp(command, "selfcheck") == 0) {
	amstar_selfcheck(&argument);
    } else if (strcmp(command, "estimate") == 0) {
	amstar_estimate(&argument);
    } else if (strcmp(command, "backup") == 0) {
	amstar_backup(&argument);
    } else if (strcmp(command, "restore") == 0) {
	amstar_restore(&argument);
    } else {
	fprintf(stderr, "Unknown command `%s'.\n", command);
	exit (1);
    }
    return 0;
}

static void
amstar_support(
    application_argument_t *argument)
{
    (void)argument;
    fprintf(stdout, "CONFIG YES\n");
    fprintf(stdout, "HOST YES\n");
    fprintf(stdout, "DISK YES\n");
    fprintf(stdout, "MAX-LEVEL 9\n");
    fprintf(stdout, "INDEX-LINE YES\n");
    fprintf(stdout, "INDEX-XML YES\n");
    fprintf(stdout, "MESSAGE-LINE YES\n");
    fprintf(stdout, "MESSAGE-XML YES\n");
    fprintf(stdout, "RECORD YES\n");
    fprintf(stdout, "INCLUDE-FILE NO\n");
    fprintf(stdout, "INCLUDE-LIST NO\n");
    fprintf(stdout, "EXCLUDE-FILE YES\n");
    fprintf(stdout, "EXCLUDE-LIST YES\n");
    fprintf(stdout, "COLLECTION NO\n");
}

static void
amstar_selfcheck(
    application_argument_t *argument)
{
    GSList *prop_values;
    char   *qdisk;
    char   *qdevice;

    qdisk = quote_string(argument->dle.disk);
    qdevice = quote_string(argument->dle.device);
    fprintf(stdout, "OK %s\n", qdisk);
    fprintf(stdout, "OK %s\n", qdevice);

    if ((prop_values = g_hash_table_lookup(argument->property,
					   "INCLUDE-FILE"))) {
	fprintf(stdout, _("ERROR amstar: Invalid INCLUDE-FILE property for %s\n"),
		qdisk);
    }
    if ((prop_values = g_hash_table_lookup(argument->property,
					   "INCLUDE-LIST"))) {
	fprintf(stdout, _("ERROR amstar: Invalid INCLUDE-LIST property for %s\n"),
		qdisk);
    }

    check_file(star_path, X_OK);

    {
	char *amandates_file;
	amandates_file = getconf_str(CNF_AMANDATES);
	check_file(amandates_file, R_OK|W_OK);
    }

}

static void
amstar_estimate(
    application_argument_t *argument)
{
    char **my_argv;
    char  *cmd;
    int    nullfd;
    int    pipefd;
    FILE  *dumpout = NULL;
    off_t  size = -1;
    char  *line;
    char  *errmsg;
    char  *qdisk;
    amwait_t wait_status;
    int    starpid;
    amregex_t *rp;

    qdisk = quote_string(argument->dle.disk);

    my_argv = amstar_build_argv(argument, CMD_ESTIMATE);

    cmd = stralloc(star_path);

    start_time = curclock();

    if ((nullfd = open("/dev/null", O_RDWR)) == -1) {
	errmsg = vstrallocf(_("Cannot access /dev/null : %s"),
			     strerror(errno));
	dbprintf("%s\n", errmsg);
	goto common_exit;
    }

    starpid = pipespawnv(cmd, STDERR_PIPE, 1,
			 &nullfd, &nullfd, &pipefd, my_argv);

    dumpout = fdopen(pipefd,"r");
    if (!dumpout) {
	error(_("Can't fdopen: %s"), strerror(errno));
	/*NOTREACHED*/
    }

    size = (off_t)-1;
    while (size < 0 && (line = agets(dumpout)) != NULL) {
	if (line[0] == '\0')
	    continue;
	dbprintf("%s\n", line);
	/* check for size match */
	/*@ignore@*/
	for(rp = re_table; rp->regex != NULL; rp++) {
	    if(match(rp->regex, line)) {
		if (rp->typ == DMP_SIZE) {
		    size = ((the_num(line, rp->field)*rp->scale+1023.0)/1024.0);
		    if(size < 0.0)
			size = 1.0;             /* found on NeXT -- sigh */
		}
		break;
	    }
	}
	/*@end@*/
	amfree(line);
    }

    while ((line = agets(dumpout)) != NULL) {
	dbprintf("%s\n", line);
	amfree(line);
    }

    dbprintf(".....\n");
    dbprintf(_("estimate time for %s level %d: %s\n"),
	      qdisk,
	      argument->level,
	      walltime_str(timessub(curclock(), start_time)));
    if(size == (off_t)-1) {
	errmsg = vstrallocf(_("no size line match in %s output"), my_argv[0]);
	dbprintf(_("%s for %s\n"), errmsg, qdisk);
	dbprintf(".....\n");
    } else if(size == (off_t)0 && argument->level == 0) {
	dbprintf(_("possible %s problem -- is \"%s\" really empty?\n"),
		  my_argv[0], argument->dle.disk);
	dbprintf(".....\n");
    }
    dbprintf(_("estimate size for %s level %d: %lld KB\n"),
	      qdisk,
	      argument->level,
	      (long long)size);

    kill(-starpid, SIGTERM);

    dbprintf(_("waiting for %s \"%s\" child\n"), my_argv[0], qdisk);
    waitpid(starpid, &wait_status, 0);
    if (WIFSIGNALED(wait_status)) {
	errmsg = vstrallocf(_("%s terminated with signal %d: see %s"),
			    cmd, WTERMSIG(wait_status), dbfn());
    } else if (WIFEXITED(wait_status)) {
	if (WEXITSTATUS(wait_status) != 0) {
	    errmsg = vstrallocf(_("%s exited with status %d: see %s"),
			        cmd, WEXITSTATUS(wait_status), dbfn());
	} else {
	    /* Normal exit */
	}
    } else {
	errmsg = vstrallocf(_("%s got bad exit: see %s"),
			    cmd, dbfn());
    }
    dbprintf(_("after %s %s wait\n"), my_argv[0], qdisk);

common_exit:

    amfree(my_argv);
    amfree(qdisk);
    amfree(cmd);

    aclose(nullfd);
    afclose(dumpout);

    fprintf(stdout, "%lld 1\n", (long long)size);
}

static void
amstar_backup(
    application_argument_t *argument)
{
    int dumpin;
    char *cmd = NULL;
    char *qdisk;
    char *line;
    amregex_t *rp;
    off_t dump_size = -1;
    char *type;
    char startchr;
    char **my_argv;
    int starpid;
    int dataf = 1;
    int mesgf = 2;
    int indexf = 3;
    int outf;
    FILE *mesgstream;
    FILE *indexstream;
    FILE *outstream;

    qdisk = quote_string(argument->dle.disk);

    my_argv = amstar_build_argv(argument, CMD_BACKUP);

    cmd = stralloc(star_path);

    starpid = pipespawnv(cmd, STDIN_PIPE|STDERR_PIPE, 1,
			 &dumpin, &dataf, &outf, my_argv);

    /* close the write ends of the pipes */
    aclose(dumpin);
    aclose(dataf);
    indexstream = fdopen(indexf, "w");
    mesgstream = fdopen(mesgf, "w");
    outstream = fdopen(outf, "r");

    while ((line = agets(outstream)) != NULL) {
	regmatch_t regmatch[3];
	regex_t regex;
        int got_match = 0;

	regcomp(&regex, "^a \\.\\/ directory$", REG_EXTENDED|REG_NEWLINE);
	if (regexec(&regex, line, 1, regmatch, 0) == 0) {
	    got_match = 1;
	    if (argument->dle.create_index)
		fprintf(indexstream, "%s\n", "/\n");
	}
	regfree(&regex);

	regcomp(&regex, "^a (.*) directory$", REG_EXTENDED|REG_NEWLINE);
	if (regexec(&regex, line, 3, regmatch, 0) == 0) {
	    got_match = 1;
	    if (argument->dle.create_index && regmatch[1].rm_so == 2) {
		line[regmatch[1].rm_eo+1]='\0';
		fprintf(indexstream, "/%s\n", &line[regmatch[1].rm_so]);
	    }
	}
	regfree(&regex);

	regcomp(&regex, "^a (.*) (.*) bytes", REG_EXTENDED|REG_NEWLINE);
	if (regexec(&regex, line, 3, regmatch, 0) == 0) {
	    got_match = 1;
	    if (argument->dle.create_index && regmatch[1].rm_so == 2) {
		line[regmatch[1].rm_eo]='\0';
		fprintf(indexstream, "/%s\n", &line[regmatch[1].rm_so]);
	    }
	}
	regfree(&regex);

	regcomp(&regex, "^a (.*) special", REG_EXTENDED|REG_NEWLINE);
	if (regexec(&regex, line, 3, regmatch, 0) == 0) {
	    got_match = 1;
	    if (argument->dle.create_index && regmatch[1].rm_so == 2) {
		line[regmatch[1].rm_eo]='\0';
		fprintf(indexstream, "/%s\n", &line[regmatch[1].rm_so]);
	    }
	}
	regfree(&regex);

	regcomp(&regex, "^a (.*) symbolic", REG_EXTENDED|REG_NEWLINE);
	if (regexec(&regex, line, 3, regmatch, 0) == 0) {
	    got_match = 1;
	    if (argument->dle.create_index && regmatch[1].rm_so == 2) {
		line[regmatch[1].rm_eo]='\0';
		fprintf(indexstream, "/%s\n", &line[regmatch[1].rm_so]);
	    }
	}
	regfree(&regex);

	if (got_match == 0) { /* message */
	    for(rp = re_table; rp->regex != NULL; rp++) {
		if(match(rp->regex, line)) {
		    break;
		}
	    }
	    if(rp->typ == DMP_SIZE) {
		dump_size = (long)((the_num(line, rp->field)* rp->scale+1023.0)/1024.0);
	    }
	    switch(rp->typ) {
	    case DMP_NORMAL:
		type = "normal";
		startchr = '|';
		break;
	    case DMP_STRANGE:
		type = "strange";
		startchr = '?';
		break;
	    case DMP_SIZE:
		type = "size";
		startchr = '|';
		break;
	    case DMP_ERROR:
		type = "error";
		startchr = '?';
		break;
	    default:
		type = "unknown";
		startchr = '!';
		break;
	    }
	    dbprintf("%3d: %7s(%c): %s\n", rp->srcline, type, startchr, line);
	    fprintf(mesgstream,"%c %s\n", startchr, line);
        }
    }

    dbprintf(_("gnutar: %s: pid %ld\n"), cmd, (long)starpid);

    dbprintf("sendbackup: size %lld\n", (long long)dump_size);
    fprintf(mesgstream, "sendbackup: size %lld\n", (long long)dump_size);
    dbprintf("sendbackup: end\n");
    fprintf(mesgstream, "sendbackup: end\n");

    fclose(mesgstream);
    if (argument->dle.create_index)
	fclose(indexstream);

    amfree(qdisk);
    amfree(cmd);
}

static void
amstar_restore(
    application_argument_t *argument)
{
    char  *cmd;
    char **my_argv;
    char **env;
    int    i, j;
    char  *e;

    if (!star_path) {
	error(_("STAR-PATH not defined"));
    }

    cmd = stralloc(star_path);
    my_argv = alloc(SIZEOF(char *) * (11 + argument->argc));
    i = 0;
    my_argv[i++] = stralloc(star_path);
    my_argv[i++] = stralloc("-x");
    my_argv[i++] = stralloc("-v");
    my_argv[i++] = stralloc("-xattr");
    my_argv[i++] = stralloc("-acl");
    my_argv[i++] = stralloc("errctl=WARN|SAMEFILE|SETTIME|DIFF|SETACL|SETXATTR|SETMODE|BADACL *");
    my_argv[i++] = stralloc("-no-fifo");
    my_argv[i++] = stralloc("-f");
    my_argv[i++] = stralloc("-");

    for (j=1; j< argument->argc; j++)
	my_argv[i++] = stralloc(argument->argv[j]+2); /* remove ./ */

    dup2(1,0);
    dup2(2,1);
    env = safe_env();
    become_root();
    execve(cmd, my_argv, env);
    e = strerror(errno);
    error(_("error [exec %s: %s]"), cmd, e);

}

char **amstar_build_argv(
    application_argument_t *argument,
    int   command)
{
    int    i;
    char  *dirname;
    char  *fsname;
    char  levelstr[NUM_STR_SIZE+7];
    char **my_argv;
    char *s;
    char *tardumpfile;

    dirname = amname_to_dirname(argument->dle.device);
    fsname = vstralloc("fs-name=", dirname, NULL);
    for (s = fsname; *s != '\0'; s++) {
	if (iscntrl((int)*s))
	    *s = '-';
    }
    snprintf(levelstr, SIZEOF(levelstr), "-level=%d", argument->level);

    if (star_dle_tardumps) {
	char *sdisk = sanitise_filename(argument->dle.disk);
	tardumpfile = vstralloc(star_tardumps, sdisk, NULL);
	amfree(sdisk);
    } else {
	tardumpfile = stralloc(star_tardumps);
    }

    my_argv = alloc(SIZEOF(char *) * 32);
    i = 0;
    
    my_argv[i++] = star_path;

    my_argv[i++] = stralloc("-c");
    my_argv[i++] = stralloc("-f");
    if (command == CMD_ESTIMATE) {
	my_argv[i++] = stralloc("/dev/null");
    } else {
	my_argv[i++] = stralloc("-");
    }
    my_argv[i++] = stralloc("-C");
#if defined(__CYGWIN__)
    {
	char tmppath[PATH_MAX];

	cygwin_conv_to_full_posix_path(dirname, tmppath);
	my_argv[i++] = stralloc(tmppath);
    }
#else
    my_argv[i++] = stralloc(dirname);
#endif
    my_argv[i++] = stralloc(fsname);
    if (star_onefilesystem)
	my_argv[i++] = stralloc("-xdev");
    my_argv[i++] = stralloc("-link-dirs");
    my_argv[i++] = stralloc(levelstr);
    my_argv[i++] = stralloc2("tardumps=", tardumpfile);
    if (command == CMD_BACKUP)
	my_argv[i++] = stralloc("-wtardumps");
    my_argv[i++] = stralloc("-xattr");
    my_argv[i++] = stralloc("-acl");
    my_argv[i++] = stralloc("H=exustar");
    my_argv[i++] = stralloc("errctl=WARN|SAMEFILE|DIFF|GROW|SHRINK|SPECIALFILE|GETXATTR|BADACL *");
    if (star_sparse)
	my_argv[i++] = stralloc("-sparse");
    my_argv[i++] = stralloc("-dodesc");

    if (command == CMD_BACKUP && argument->dle.create_index)
	my_argv[i++] = stralloc("-v");

    my_argv[i++] = stralloc(".");

    my_argv[i] = NULL;

    amfree(tardumpfile);
    amfree(fsname);
    amfree(dirname);

    return(my_argv);
}