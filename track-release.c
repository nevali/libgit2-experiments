/* This is a utility, intended to be invoked in a post-receive hook, which
 * maintains a SQLite3 database of releases.
 *
 * Releases are defined in one of two ways, depending upon how things are
 * configured.
 *
 * Each configured branch is intended to map to a package repository. So,
 * you might have a 'master' branch whose releases are pushed into an
 * integration repository. For that, you would set the release tracking mode to
 * 'tip' - which means every time this utility sees that the tip of the
 * branch points to a new commit, it will generate a version number for it
 * and add it to the database.
 *
 * Meanwhile, you might have 'testing' and 'live' branches, for which you
 * only want to build tagged releases. For this, you would set the release
 * tracking mode to 'tag'. For each tag-tracked branch, the history is
 * walked to see if there are any commits matching tags whose names look
 * like a version number. If so, it will add it to the database. Of course,
 * the same tag might point at a commit which exists in multiple
 * release-tracked branches; in which case, the version will be added to
 * the database against both branches.
 *
 * The database consists of a single table, "releases", which is defined as:
 *
 *   "release"    (string)   The version number
 *   "commit"     (string)   The full 40-character OID of the commit
 *   "branch"     (string)   The name of the branch/package repository
 *   "when"       (datetime) The timestamp of the commit
 *   "added"      (datetime) The timestamp that the release was added
 *   "state"      (string)   The state of the release, initially "NEW"
 *   "built"      (datetime) The timestamp that the release was built
 *
 * The primary key of the table is (release, branch).
 *
 * This utility will always add new rows with a state of "NEW" and a build
 * date of NULL. It will never update them itself: they're intended to assist
 * something else in actually triggering/performing builds.
 *
 * If a (release, branch) row exists but the commit OID differs, the existing
 * entry will be removed and added afresh (i.e., because the tag was deleted
 * and re-created in between pushes).
 *
 * This utility does not detect if a tag is deleted, because there is little
 * value in doing so -- although it would be fairly straightforward to add
 * if desirable.
 *
 * Per-branch configuration looks like this:
 *
 * [release-branch "master"]
 * track = tip
 *
 * [release-branch "stable"]
 * track = tag
 *
 * Branches without a release-branch...track configuration setting are
 * ignored.
 *
 * Branch names must consist of letters, numbers, hyphens and underscores
 * in order to be release-tracked.
 *
 * Tag names for tips have the form YYMM.DDHH.MMSS-gitXXXXXXX (where XXXXXXX
 * is the shortened OID of the commit).
 *
 * For tag-tracked branches, the tag must be in the form:
 *
 * <major>.<minor>...
 * r<major>.<minor>...
 * v<major>.<minor>...
 * debian/<major>.<minor>...
 * release/<major>.<minor>...
 *
 * The <major> part must be all-numeric.
 * The <minor> part must begin with a digit. The remainder must consist only
 * of letters, numbers, dashes, underscores, full stops and tildes.
 *
 * Tags which do not match any of the above patterns are silently ignored.
 *
 * The releases database is named 'releases.sqlite3' and is created within
 * $GIT_DIR (by default the root of a bare repository, or in the '.git'
 * directory in a non-bare repository). Any SQLite3 client (including the
 * 'sqlite3' command-line utility) should be able to open and inspect it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <alloca.h>
#include <errno.h>

#include "utils.h"

static char *sqlbuf;
static size_t sqlbuflen;

struct tag_match_struct
{
	/* The repository we're matching against */
	REPO *repo;
	/* The OID to match (in iterator callbacks) */
	git_oid oidmatch;
	/* The current branch name */
	const char *branch_name;
};

struct release_match_struct
{
	/* The formatted OID to match */
	char oidstr[GIT_OID_HEXSZ+1];
	/* The matching result:
	 *   0 = not found, 1 = found, 2 = found but OID differs
	 */
	int result;
};

struct hook_data_struct
{
	/* The repository */
	REPO *repo;
	/* The path to the hook function to execute */
	char *path;
};

/* Perform a SQL query, terminating the application if it fails */
static int
sql_exec(REPO *repo, const char *sql)
{
	char *err;

	err = NULL;
	if(sqlite3_exec(repo->db, sql, NULL, NULL, &err) != SQLITE_OK)
	{
		fprintf(stderr, "%s: %s\n", repo->progname, err);
		fprintf(stderr, "%s: while executing '%s'\n", repo->progname, sql);
		exit(EXIT_FAILURE);	  
	}
	return 0;
}

static int
release_exists_cb(void *data, int ncols, char **values, char **columns)
{
	struct release_match_struct *match;

	(void) columns;

	match = (struct release_match_struct *) data;
	if(match->result)
	{
		return 0;
	}
	if(ncols >= 1 && values[0] && !strcmp(values[0], match->oidstr))
	{		
		match->result = 1;
	}
	else
	{
		match->result = 2;
	}	
	return 0;
}

/* Check if a release already exists in the database
 * If it's present but its commit doesn't match the provided OID, delete the
 * entry from the list and return zero so that it's added afresh.
 */
static int
release_exists(REPO *repo, const char *version, const char *branch_name, const char *oidstr)
{
	struct release_match_struct match;
	char *err;
   
	strcpy(match.oidstr, oidstr);
	match.result = 0;
	snprintf(sqlbuf, sqlbuflen, "SELECT \"commit\" FROM \"releases\" WHERE \"release\" = '%s' AND \"branch\" = '%s'", version, branch_name);	
	err = NULL;
	if(sqlite3_exec(repo->db, sqlbuf, release_exists_cb, (void *) &match, &err))
	{
		fprintf(stderr, "%s: %s\n", repo->progname, err);
		exit(EXIT_FAILURE);
	}
	/* Either no matching records, or an exact match for the OID */
	if(match.result < 2)
	{
		return match.result;
	}
	/* A match found, but the OID differs -- delete the old one */
	sprintf(sqlbuf, "DELETE FROM \"releases\" WHERE \"release\" = '%s' AND \"branch\" = '%s'", version, branch_name);
	sql_exec(repo, sqlbuf);
	return 0;
}

/* Add a release */
static int
add_release(REPO *repo, const char *branch_name, const git_oid *oid, const char *version, struct tm *when)
{
	char oidstr[GIT_OID_HEXSZ+1];
	char datebuf[32], datebuf2[32];
	time_t t;
	struct tm now;
	
	t = time(NULL);
	gmtime_r(&t, &now);
	strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M:%S", when);
	strftime(datebuf2, sizeof(datebuf2), "%Y-%m-%d %H:%M:%S", &now);
	git_oid_fmt(oidstr, oid);
	oidstr[GIT_OID_HEXSZ] = 0;
	sql_exec(repo, "BEGIN");
	if(release_exists(repo, version, branch_name, oidstr))
	{
		sql_exec(repo, "ROLLBACK");
		return 0;
	}
	sprintf(sqlbuf, "INSERT INTO \"releases\" (\"release\", \"branch\", \"commit\", \"when\", \"added\", \"state\") VALUES ('%s', '%s', '%s', '%s', '%s', '%s')", version, branch_name, oidstr, datebuf, datebuf2, "NEW");
	oidstr[8] = 0;
	fprintf(stderr, "%s: added %s as %s on %s\n", repo->progname, oidstr, version, branch_name);
	sql_exec(repo, sqlbuf);
	sql_exec(repo, "COMMIT");
	return 0;
}

/* Add a 'tip' release */
static int
add_release_tip(REPO *repo, const char *branch_name, const git_oid *oid)
{
	char versbuf[32];
	git_commit *commit;
	const git_signature *sig;
	char oidstr[GIT_OID_HEXSZ+1];
	struct tm tm;

	(void) repo;

	commit = NULL;
	git_oid_fmt(oidstr, oid);
	if(git_commit_lookup(&commit, repo->repo, oid))
	{
		fprintf(stderr, "%s: failed to locate commit %s as tip of branch '%s'\n", repo->progname, oidstr, branch_name);
		return -1;
	}
	oidstr[8] = 0;
	sig = git_commit_committer(commit);
	gmgittime(&(sig->when), &tm, NULL, NULL, NULL);
	strftime(versbuf, sizeof(versbuf), "%y%m.%d%H.%M%S-git", &tm);
	strcat(versbuf, oidstr);
	return add_release(repo, branch_name, oid, versbuf, &tm);
}

static int
tag_callback(const char *tag_name, git_oid *oid, void *data)
{
	struct tag_match_struct *match;
	git_commit *commit;
	const git_signature *sig;
	struct tm tm;
	const char *version;

	match = (struct tag_match_struct *) data;
	if(git_oid_cmp(oid, &(match->oidmatch)))
	{
		return 0;
	}
	version = check_release_tag(tag_name);
	if(!version)
	{
		return 0;
	}
	if(git_commit_lookup(&commit, match->repo->repo, oid))
	{
		fprintf(stderr, "%s: failed to locate commit for tag '%s'\n", match->repo->progname, tag_name);
		return 1;
	}
	sig = git_commit_committer(commit);
	gmgittime(&(sig->when), &tm, NULL, NULL, NULL);	
	add_release(match->repo, match->branch_name, oid, version, &tm);
	return 1;
}

static int
branch_callback(git_reference *ref, const char *branch_name, git_branch_t branch_type, void *data)
{
	REPO *repo;
	struct tag_match_struct tagmatch;
	const char *cfgval, *t;
	char *p;
	const git_oid *oid;
	git_oid oidbuf;
	git_revwalk *walker;
	
	(void) branch_type;

	repo = (REPO *) data;
	git_branch_name(&branch_name, ref);
	t = check_release_branch(branch_name);
	if(!t)
	{
		fprintf(stderr, "%s: ignoring branch '%s' because its name is not valid for release-tracking\n", repo->progname, branch_name);
		return 0;
	}
	memset(&tagmatch, 0, sizeof(tagmatch));
	tagmatch.repo = repo;
	tagmatch.branch_name = t;
	p = alloca(strlen(t) + 32);
	strcpy(p, "release-branch.");
	strcat(p, t);
	strcat(p, ".track");
	cfgval = NULL;
	git_config_get_string(&cfgval, repo->cfg, p);
	if(cfgval)
	{
		if(!strcmp(cfgval, "tip"))
		{
			/* Add the commit at the tip of the branch as a release */
			oid = git_reference_target(ref);
			if(oid)
			{
				add_release_tip(repo, t, oid);
			}
		}
		else if(!strcmp(cfgval, "tag"))
		{
			/* Walk the history of the branch, matching commits with tags which
			 * look like releases.
			 */
			oid = git_reference_target(ref);
			git_oid_cpy(&oidbuf, oid);
			git_revwalk_new(&walker, repo->repo);
			git_revwalk_sorting(walker, GIT_SORT_TOPOLOGICAL);
			git_revwalk_push(walker, &oidbuf);
			while(!git_revwalk_next(&oidbuf, walker))
			{
				/* Now attempt to find a tag for the commit */
				git_oid_cpy(&(tagmatch.oidmatch), &oidbuf);
				git_tag_foreach(repo->repo, tag_callback, (void *) &tagmatch);
			}
			git_revwalk_free(walker);
		}
		else
		{
			fprintf(stderr, "%s: warning: tracking mode '%s' (for branch '%s') is not supported\n", repo->progname, cfgval, t);
		}
	}
	return 0;
}

static int
build_release_cb(void *data, int ncols, char **values, char **columns)
{
	struct hook_data_struct *hook;
	int r;
	char *args[5];
	char resultbuf[64];

	hook = (struct hook_data_struct *) data;

	(void) columns;
	(void) ncols;

	fprintf(stderr, "%s: will build '%s' for '%s' as '%s'\n", hook->repo->progname, values[0], values[1], values[2]);
	args[0] = hook->path;
	args[1] = values[0]; /* Commit */
	args[2] = values[1]; /* Branch */
	args[3] = values[2]; /* Version */
	args[4] = NULL;
	r = spawn(hook->path, args);
	if(r == 0)
	{
		strcpy(resultbuf, "SUCCESS");
	}
	else
	{
		snprintf(resultbuf, sizeof(resultbuf), "FAILED (%d)", r);
	}
	snprintf(sqlbuf, sqlbuflen, "UPDATE \"releases\" SET \"state\" = '%s' WHERE \"release\" = '%s' AND \"branch\" = '%s'",
			 resultbuf, values[2], values[1]);
	sql_exec(hook->repo, sqlbuf);
	fprintf(stderr, "%s: build status is: %s\n", hook->repo->progname, resultbuf);
	return 0;
}

static void
usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [PATH-TO-REPO]\nHonours GIT_DIR if set.\n", progname);
}

int
main(int argc, char **argv)
{
	const char *path;
	REPO *repo;
	git_branch_iterator *branch_iter;
	git_branch_t branch_type;
	git_reference *ref;
	int c;
	char *err, *p;
	struct hook_data_struct hook;

	path = NULL;
	while((c = getopt(argc, argv, "h")) != -1)
	{
		switch(c)
		{
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}
	if(argc - optind > 1)
	{
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	if(argc - optind > 0)
	{
		path = argv[1];
	}
	repo = repo_open(argv[0], path, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, 1);
	if(!repo)
	{
		exit(EXIT_FAILURE);
	}
	sqlbuflen = 1024;
	sqlbuf = (char *) xalloc(sqlbuflen + 1);
	
	sql_exec(repo,
			 "CREATE TABLE IF NOT EXISTS \"releases\" ( "
			 "  \"release\" VARCHAR(32) NOT NULL, "
			 "  \"commit\" CHAR(40) NOT NULL, "
			 "  \"branch\" VARCHAR(32) NOT NULL, "
			 "  \"when\" DATETIME NOT NULL, "
			 "  \"added\" DATETIME NOT NULL, "
			 "  \"state\" VARCHAR(16) NOT NULL, "
			 "  \"built\" DATETIME DEFAULT NULL, "
			 "  PRIMARY KEY (\"release\", \"branch\") "
			 ")");
	
	git_branch_iterator_new(&branch_iter, repo->repo, GIT_BRANCH_LOCAL);
	while(git_branch_next(&ref, &branch_type, branch_iter) == 0)
	{
		branch_callback(ref, git_reference_name(ref), branch_type, (void *) repo);
	}
	git_branch_iterator_free(branch_iter);

	/* Iterate each of the releases and invoke the 'release' hook */
	memset(&hook, 0, sizeof(hook));
	hook.repo = repo;
	hook.path = (char *) xalloc(strlen(repo->path) + 32);
	strcpy(hook.path, repo->path);
	p = strchr(hook.path, 0);
	if(p > hook.path)
	{
		p--;
		if(*p != '/')
		{
			p++;
			*p = '/';
			p++;
			*p = 0;
		}
		else
		{
			p++;
		}
	}
	strcpy(p, "hooks/release");
	if(!access(hook.path, R_OK|X_OK))
	{
		err = NULL;
		if(sqlite3_exec(repo->db, "SELECT \"commit\", \"branch\", \"release\" FROM \"releases\" WHERE \"state\" = 'NEW'", build_release_cb, (void *) &hook, &err))
		{
			fprintf(stderr, "%s: %s\n", repo->progname, err);
			exit(EXIT_FAILURE);
		}
	}
	free(hook.path);
	free(sqlbuf);
	repo_close(repo);
	return 0;
}
