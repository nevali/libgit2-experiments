#ifndef UTILS_H_
# define UTILS_H_                       1

# include <time.h>
# include <sys/types.h>

# include <git2.h>
# include <sqlite3.h>

typedef struct repo_struct REPO;

struct repo_struct
{
	/* The program name, for error messages */
	char *progname;
	/* The libgit2 repository object */
	git_repository *repo;
	/* The libgit2 configuration dictionary */
	git_config *cfg;
	/* The path to the repository */
	char *path;
	/* The name of the repository (or the package it contains) */
	char *name;
	/* The buffer to hold a discovered repository path */
	git_buf pathbuf;
	/* The path to the SQLite3 database */
	char *dbpath;
	/* The SQLite3 database object */
	sqlite3 *db;
};	

/* Allocate a buffer, aborting if allocation fails */
void *xalloc(size_t size);
/* Duplicate a string, aborting if allocation fails */
char *xstrdup(const char *src);
/* Re-allocate a buffer, aborting if re-allocation fails */
void *xrealloc(void *ptr, size_t newsize);

/* Open a repository, the configuration dictionary, and the releases database */
REPO *repo_open(const char *progname, const char *repopath, int sqliteflags, int requiredb);
/* Close a repository, freeing resources */
int repo_close(REPO *repo);

/* Check if a given tag name is a release tag, returning a pointer to the
 * start of the version number if so, or NULL if not
 */
const char *check_release_tag(const char *tag_name);
/* Check the name of a branch to ensure it's something we consider valid
 * as a release-tracking branch name
 */
const char *check_release_branch(const char *branch_name);
/* Convert a git_time to a UTC struct tm and accompanying hours/minutes
 * offset and sign
 */
int gmgittime(const git_time *time, struct tm *tm, int *hours, int *minutes, char *signptr);
/* Spawn a process with sensible defaults and wait for it to complete */
int spawn(const char *pathname, char *const *argv);

#endif /*!UTILS_H_*/
