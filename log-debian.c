#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include "utils.h"

/* Output a changelog in Debian format:

package (version) branch; urgency=low

  * Log entry.
  * Log entry.
  * Log entry.

 -- Name <email@address>  Day, DD Mon Year HH:MM:SS +ZZZZ

*/

struct tag_match_struct
{
	REPO *repo;
	const git_oid *oid;
	char *buf;
	size_t buflen;
};

static void
usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [OPTIONS] BRANCH [PATH-TO-REPO]\nHonours GIT_DIR if set. OPTIONS is one or more of:\n", progname);
	fprintf(stderr,
			"  -h            Print this usage message and exit\n"
			"  -c COMMITID   Begin the log at this commit. If the commit does not appear\n"
			"                on this branch or doesn't correspond to a release, an error\n"
			"                will be reported.\n");
}

static int
tag_callback(const char *tag_name, git_oid *oid, void *data)
{
	struct tag_match_struct *match;
	const char *t;
	
	match = (struct tag_match_struct *) data;
	t = check_release_tag(tag_name);
	if(!t)
	{
		return 0;
	}
	if(git_oid_cmp(oid, match->oid))
	{
		return 0;
	}
	strncpy(match->buf, t, match->buflen);
	match->buf[match->buflen - 1] = 0;
	return 1;
}

static int
release_exists_cb(void *data, int ncols, char **values, char **columns)
{
	struct tag_match_struct *match;

	(void) ncols;
	(void) columns;

	match = (struct tag_match_struct *) data;
	
	if(values[0])
	{
		strncpy(match->buf, values[0], match->buflen);
		match->buf[match->buflen - 1] = 0;
	}
	return 0;
}

static const char *
commit_is_release(REPO *repo, git_commit *commit, const char *branchname)
{
	static char version[64];
	const git_oid *id;
	char oidstr[GIT_OID_HEXSZ+1];
	struct tag_match_struct match;
	char sqlbuf[256];
	char *err;

	if(!commit)
	{
		return NULL;
	}
	id = git_commit_id(commit);  
	version[0] = 0;
	match.repo = repo;
	match.buf = version;
	match.buflen = sizeof(version);
	if(repo->db)
	{
		git_oid_fmt(oidstr, id);
		oidstr[GIT_OID_HEXSZ] = 0;
		sprintf(sqlbuf, "SELECT \"release\" FROM \"releases\" WHERE \"branch\" = '%s' AND \"commit\" = '%s'", branchname, oidstr);
		err = NULL;
		if(sqlite3_exec(repo->db, sqlbuf, release_exists_cb, (void *) &match, &err))
		{
			fprintf(stderr, "%s: %s\n", repo->progname, err);
			exit(EXIT_FAILURE);
		}
		if(version[0])
		{
			return version;
		}
		return NULL;
	}
	match.oid = id;

	git_tag_foreach(repo->repo, tag_callback, (void *) &match);
	if(version[0])
	{
		return version;
	}
	return NULL;
}

static int
log_commit_message(const char *message)
{
	int nl;
	
	nl = 1;
	while(*message)
	{
		if(nl)
		{
			while(isspace(*message))
			{
				message++;
			}
			if(!*message)
			{
				break;
			}
			putchar(' ');
			putchar(' ');
			putchar('*');
			putchar(' ');
			nl = 0;
		}
		if(*message == '\n')
		{
			putchar('\n');
			nl = 1;
			message++;
			continue;
		}
		putchar(*message);
		message++;
	}
	if(!nl)
	{
		putchar('\n');
	}
	return 0;
}

static int
log_commit(REPO *repo, git_commit *commit, const char *branchname)
{
	static const git_signature *relsig;
	const git_signature *sig;
	struct tm tm;
	int hours, minutes;
	char sign, datebuf[64];
	const char *vers;
	
	vers = commit_is_release(repo, commit, branchname);
	if(!commit || vers)
	{
		if(relsig)
		{
			gmgittime(&(relsig->when), &tm, &hours, &minutes, &sign);
			strftime(datebuf, sizeof(datebuf), "%a, %e %b %Y %H:%M:%S", &tm);
			printf("\n -- %s <%s>  %s %c%02d%02d\n", relsig->name, relsig->email, datebuf, sign, hours, minutes);
			relsig = NULL;
			if(commit)
			{
				putchar('\n');
			}
		}
	}
	if(!commit)
	{
		return 0;
	}
	if(!relsig && !vers)
	{
		/* We haven't yet reached a release */
		return 0;
	}
	sig = git_commit_committer(commit);
	if(!relsig)
	{
		relsig = sig;
		printf("%s (%s) %s; urgency=low\n\n", repo->name, vers, branchname);
	}
	log_commit_message(git_commit_message(commit));
	return 1;
}

int
main(int argc, char **argv)
{
	const char *path, *branch, *startcommit;
	const git_error *err;
	const git_oid *tip;
	git_object *startobj;
	git_oid oid, startoid;
	git_reference *ref;
	git_revwalk *walker;
	git_commit *commit;
	char oidstr[GIT_OID_HEXSZ+1];
	REPO *repo;
	int c, started;

	startcommit = NULL;
	while((c = getopt(argc, argv, "hc:")) != -1)
	{	
		switch(c)
		{
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		case 'c':
			startcommit = optarg;
			break;
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}
	if(argc - optind < 1 || argc - optind > 2)
	{
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	branch = argv[optind];
	path = NULL;
	if(argc - optind > 1)
	{
		path = argv[optind + 1];
	}
	repo = repo_open(argv[0], path, SQLITE_OPEN_READONLY, 0);
	if(!repo)
	{
		exit(EXIT_FAILURE);
	}
	/* If there's a starting commit, find its OID */
	if(startcommit)
	{
		startobj = NULL;
		if(git_revparse_single(&startobj, repo->repo, startcommit))
		{
			err = giterr_last();
			fprintf(stderr, "%s: %s\n", repo->progname, err->message);
			repo_close(repo);
			exit(EXIT_FAILURE);
		}
		if(git_object_type(startobj) != GIT_OBJ_COMMIT)
		{
			fprintf(stderr, "%s: unable to find a commit for '%s'\n", repo->progname, startcommit);
			git_object_free(startobj);
			repo_close(repo);
			exit(EXIT_FAILURE);
		}
		git_oid_cpy(&startoid, git_commit_id((git_commit *) startobj));
		git_object_free(startobj);
	}
	/* Look the target branch up */
	if(git_branch_lookup(&ref, repo->repo, branch, GIT_BRANCH_LOCAL))
	{
		err = giterr_last();
		fprintf(stderr, "%s: %s\n", repo->progname, err->message);
		repo_close(repo);
		exit(EXIT_FAILURE);	
	}

	/* Obtain the canonical branch name */
	git_branch_name(&branch, ref);
	/* Find the tip of the branch */
	tip = git_reference_target(ref);
	git_oid_cpy(&oid, tip);
	/* Create a walker for the log entries for this branch */
	git_revwalk_new(&walker, repo->repo);
	git_revwalk_sorting(walker, GIT_SORT_TOPOLOGICAL);
	git_revwalk_push(walker, &oid);
	if(startcommit)
	{
		/* Wait until we find the requested commit before logging
		 * releases.
		 */
		started = 0;
	}
	else
	{
		/* Log all releases on the branch */
		started = 1;
	}		
	while(!git_revwalk_next(&oid, walker))
	{
		if(git_commit_lookup(&commit, repo->repo, &oid))
		{
			err = giterr_last();
			fprintf(stderr, "%s: %s\n", repo->progname, err->message);
			repo_close(repo);
			exit(EXIT_FAILURE);	
		}
		if(!started)
		{
			if(git_oid_cmp(&oid, &startoid))
			{
				continue;
			}
			/* Don't set 'started' until after the commit has been
			 * logged and we know that it actually corresponded to
			 * a release.
			 */
		}
		if(!log_commit(repo, commit, branch) && !started)
		{
			/* The requested starting commit did appear on the branch,
			 * but didn't correspond to a release, which we consider to
			 * be an error.
			 */
			git_oid_fmt(oidstr, &startoid);
			oidstr[GIT_OID_HEXSZ] = 0;
			fprintf(stderr, "%s: commit '%s' is not a release on '%s'\n", repo->progname, oidstr, branch);
			git_revwalk_free(walker);
			repo_close(repo);
			exit(EXIT_FAILURE);
		}
		started = 1;
	}
	log_commit(repo, NULL, NULL);
	git_revwalk_free(walker);
	if(!started)
	{
		git_oid_fmt(oidstr, &startoid);
		oidstr[GIT_OID_HEXSZ] = 0;
		fprintf(stderr, "%s: commit '%s' does not appear on branch '%s'\n", repo->progname, oidstr, branch);
		repo_close(repo);
		exit(EXIT_FAILURE);
	}	
	repo_close(repo);
	return 0;
}
