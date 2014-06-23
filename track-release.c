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

#include <sqlite3.h>
#include <git2.h>

struct repo_data_struct
{
	/* The program name, for error messages */
	const char *progname;
	/* The libgit2 repository object */
	git_repository *repo;
	/* The libgit2 configuration dictionary */
	git_config *cfg;
	/* The path to the repository */
	const char *path;
	/* The path to the SQLite3 database */
	char *dbpath;
	/* The SQLite3 database object */
	sqlite3 *db;
	/* The current branch name */
	const char *branch_name;
	/* SQL query buffer */
	char *sqlbuf;
	/* Size of the SQL query buffer */
	size_t sqlbuflen;
	/* The OID to match (in iterator callbacks) */
	git_oid oidmatch;
	/* The formatted OID to match (in query callbacks) */
	char oidstr[GIT_OID_HEXSZ+1];
	/* Result code from iterator callbacks */
	int result;
};

/* Allocate a buffer, aborting if allocation fails */
static void *
xalloc(size_t size)
{
	void *ptr;
	
	ptr = calloc(1, size);
	if(!ptr)
	{
		abort();
	}
	return ptr;
}

/* Re-allocate a buffer, aborting if re-allocation fails */
/* [not currently used]
static void *
xrealloc(void *ptr, size_t newsize)
{
	void *newptr;

	newptr = realloc(ptr, newsize);
	if(!newptr)
	{
		abort();
	}
	return newptr;
}
*/

/* Perform a SQL query, terminating the application if it fails */
static int
sql_exec(struct repo_data_struct *repo, const char *sql)
{
	char *err;

	err = NULL;
	if(sqlite3_exec(repo->db, sql, NULL, NULL, &err) != SQLITE_OK)
	{
		fprintf(stderr, "%s: %s\n", repo->progname, err);
		exit(EXIT_FAILURE);
	}
	return 0;
}

/* Convert a git_time structure to a struct tm, along with hours/minutes
 * offsets and a timezone offset sign character ('+' or '-')
 */
static int
git_gmtime(const git_time *intime, struct tm *tm, int *hours, int *minutes, char *sign)
{
	int offset;
	time_t t;

	offset = intime->offset;
	*sign = (offset < 0 ? '-' : '+');
	if(offset < 0)
	{
		offset = -offset;
	}
	*hours = offset / 60;
	*minutes = offset % 60;
	t = (time_t) intime->time + (intime->offset * 60);
	gmtime_r(&t, tm);
	return 0;
}

static int
release_exists_cb(void *data, int ncols, char **values, char **columns)
{

	struct repo_data_struct *repo;

	(void) columns;

	repo = (struct repo_data_struct *) data;
	if(repo->result)
	{
		return 0;
	}
	if(ncols >= 1 && values[0] && !strcmp(values[0], repo->oidstr))
	{		
		repo->result = 1;
	}
	else
	{
		repo->result = 2;
	}	
	return 0;
}

/* Check if a release already exists in the database
 * If it's present but its commit doesn't match the provided OID, delete the
 * entry from the list and return zero so that it's added afresh.
 */
static int
release_exists(struct repo_data_struct *repo, const char *version, const char *branch_name, const char *oidstr)
{
	char *err;

	strcpy(repo->oidstr, oidstr);
	sprintf(repo->sqlbuf, "SELECT \"commit\" FROM \"releases\" WHERE \"release\" = '%s' AND \"branch\" = '%s'", version, branch_name);	
	repo->result = 0;
	err = NULL;
	if(sqlite3_exec(repo->db, repo->sqlbuf, release_exists_cb, (void *) repo, &err))
	{
		fprintf(stderr, "%s: %s\n", repo->progname, err);
		exit(EXIT_FAILURE);
	}
	/* Either no matching records, or an exact match for the OID */
	if(repo->result == 0 || repo->result == 1)
	{
		return repo->result;
	}
	/* A match found, but the OID differs -- delete the old one */
	sprintf(repo->sqlbuf, "DELETE FROM \"releases\" WHERE \"release\" = '%s' AND \"branch\" = '%s'", version, branch_name);
	sql_exec(repo, repo->sqlbuf);
	return 0;
}

/* Add a release */
static int
add_release(struct repo_data_struct *repo, const char *branch_name, const git_oid *oid, const char *version, struct tm *when)
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
	sprintf(repo->sqlbuf, "INSERT INTO \"releases\" (\"release\", \"branch\", \"commit\", \"when\", \"added\", \"state\") VALUES ('%s', '%s', '%s', '%s', '%s', '%s')", version, branch_name, oidstr, datebuf, datebuf2, "NEW");
	oidstr[8] = 0;
	fprintf(stderr, "%s: added %s as %s on %s\n", repo->progname, oidstr, version, branch_name);
	sql_exec(repo, repo->sqlbuf);
	sql_exec(repo, "COMMIT");
	return 0;
}

/* Add a 'tip' release */
static int
add_release_tip(struct repo_data_struct *repo, const char *branch_name, const git_oid *oid)
{
	char versbuf[32];
	git_commit *commit;
	const git_signature *sig;
	char oidstr[GIT_OID_HEXSZ+1];
	struct tm tm;
	int hours, minutes;
	char sign;

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
	git_gmtime(&(sig->when), &tm, &hours, &minutes, &sign);
	strftime(versbuf, sizeof(versbuf), "%y%m.%d%H.%M%S-git", &tm);
	strcat(versbuf, oidstr);
	return add_release(repo, branch_name, oid, versbuf, &tm);
}

static int
tag_callback(const char *tag_name, git_oid *oid, void *data)
{
	const char *t;
	struct repo_data_struct *repo;
	git_commit *commit;
	const git_signature *sig;
	int hours, minutes;
	char sign;
	struct tm tm;
	
	repo = (struct repo_data_struct *) data;	
	if(git_oid_cmp(oid, &(repo->oidmatch)))
	{
		return 0;
	}
	if(!strncmp(tag_name, "refs/tags/", 10))
	{
		tag_name += 10;
	}
	if(tolower(tag_name[0]) == 'v' || tolower(tag_name[0]) == 'r')
	{
		tag_name++;
	}
	else if(!strncmp(tag_name, "debian/", 7))
	{
		tag_name += 7;
	}
	else if(!strncmp(tag_name, "release/", 8))
	{
		tag_name+= 8;
	}
	if(!tag_name[0])
	{
		return 0;
	}
	/* Check that the remainder of the tag looks something
	 * like a version number: this means that it must consist
	 * of a string which begins with '999.9', where '999' is
	 * any number of digits.
	 */
	t= tag_name;
	while(*t && isdigit(*t))
	{
		t++;
	}
	if(*t != '.')
	{
		return 0;
	}
	t++;
	if(!isdigit(*t))
	{
		return 0;
	}
	for(; *t; t++)
	{
		if(!isalnum(*t) && *t != '-' && *t != '_' && *t != '.' && *t != '~' && *t != '@')
		{
			return 0;
		}
	}
	if(git_commit_lookup(&commit, repo->repo, oid))
	{
		fprintf(stderr, "failed to locate commit\n");
		return 1;
	}
	sig = git_commit_committer(commit);
	git_gmtime(&(sig->when), &tm, &hours, &minutes, &sign);	
	add_release(repo, repo->branch_name, oid, tag_name, &tm);
	return 1;
}

static int
branch_callback(git_reference *ref, const char *branch_name, git_branch_t branch_type, void *data)
{
	struct repo_data_struct *repo;
	const char *cfgval, *t;
	char *p;
	const git_oid *oid;
	git_oid oidbuf;
	git_revwalk *walker;
	
	(void) branch_type;

	repo = (struct repo_data_struct *) data;
	git_branch_name(&branch_name, ref);
	/* Check that the branch name is suitable */
	for(t = branch_name; *t; t++)
	{
		if(!isalnum(*t) && *t != '-' && *t != '_')
		{
			fprintf(stderr, "%s: ignoring branch '%s' because its name is not valid for release-tracking\n", repo->progname, branch_name);
			return 0;
		}
	}
	repo->branch_name = branch_name;
	p = alloca(strlen(branch_name) + 32);
	strcpy(p, "release-branch.");
	strcat(p, branch_name);
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
				add_release_tip(repo, branch_name, oid);
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
				git_oid_cpy(&(repo->oidmatch), &oidbuf);
				git_tag_foreach(repo->repo, tag_callback, (void *) repo);
			}
			git_revwalk_free(walker);
		}
		else
		{
			fprintf(stderr, "%s: warning: tracking mode '%s' (for branch '%s') is not supported\n", repo->progname, cfgval, branch_name);			
		}
	}
	repo->branch_name = NULL;
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
	git_buf pathbuf;
	const char *path;
	const git_error *err;
	struct repo_data_struct data;
	git_branch_iterator *branch_iter;
	git_branch_t branch_type;
	git_reference *ref;
	char *t;

	memset(&data, 0, sizeof(data));
	path = NULL;
	if(argc == 2)
	{
		path = argv[1];
	}
	else if(argc != 1)
	{
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	data.progname = argv[0];
	data.sqlbuflen = 1024;
	data.sqlbuf = (char *) xalloc(data.sqlbuflen);
	if(!path)
	{
		path = getenv("GIT_DIR");
	}
	memset(&pathbuf, 0, sizeof(pathbuf));
	if(!path)
	{
		if(git_repository_discover(&pathbuf, ".", 0, "/"))
		{
			err = giterr_last();
			fprintf(stderr, "%s: %s\n", path, err->message);
			exit(EXIT_FAILURE);
		}
		path = pathbuf.ptr;
	}
	if(git_repository_open(&(data.repo), path))
	{
		err = giterr_last();
		fprintf(stderr, "%s: %s\n", path, err->message);
		exit(EXIT_FAILURE);
	}
	data.path = path;
	/* Determine the release database path */
	data.dbpath = (char *) xalloc(strlen(path) + 32);
	strcpy(data.dbpath, path);
	t = strchr(data.dbpath, 0);
	if(t > data.dbpath)
	{
		t--;
		if(*t != '/')
		{
			t++;
			*t = '/';
			t++;
			*t = 0;
		}
	}
	strcat(data.dbpath, "releases.sqlite3");	
	
	/* Open the configuration dictionary */
	if(git_repository_config(&(data.cfg), data.repo))
	{
		err = giterr_last();
		fprintf(stderr, "%s: %s\n", path, err->message);
		exit(EXIT_FAILURE);
	}
	/* Open (creating if necessary) the SQLite3 release database */
	if(sqlite3_open_v2(data.dbpath, &(data.db), SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, NULL))
	{
		fprintf(stderr, "%s: %s\n", data.dbpath, sqlite3_errmsg(data.db));
		exit(EXIT_FAILURE);
	}
	sqlite3_extended_result_codes(data.db, 1);
	
	sql_exec(&data,
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
	
	git_branch_iterator_new(&branch_iter, data.repo, GIT_BRANCH_LOCAL);
	while(git_branch_next(&ref, &branch_type, branch_iter) == 0)
	{
		branch_callback(ref, git_reference_name(ref), branch_type, (void *) &data);
	}
	git_branch_iterator_free(branch_iter);

	git_config_free(data.cfg);
	git_repository_free(data.repo);
	git_buf_free(&pathbuf);
	sqlite3_close(data.db);
	free(data.dbpath);
	free(data.sqlbuf);
	return 0;
}
