#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "utils.h"

/* Allocate a buffer, aborting if allocation fails */
void *
xalloc(size_t size)
{
	void *ptr;
	
	ptr = calloc(1, size);
	if(!ptr)
	{
		fprintf(stderr, "failed to allocate %lu bytes\n", (long) size);
		abort();
	}
	return ptr;
}

/* Duplicate a string, aborting if allocation fails */
char *
xstrdup(const char *src)
{
	char *p;
	
	p = xalloc(strlen(src) + 1);
	strcpy(p, src);
	return p;
}

/* Re-allocate a buffer, aborting if re-allocation fails */
void *
xrealloc(void *ptr, size_t newsize)
{
	void *newptr;

	newptr = realloc(ptr, newsize);
	if(!newptr)
	{
		fprintf(stderr, "failed to re-allocate buffer to %lu bytes\n", (long) newsize);
		abort();
	}
	return newptr;
}

/* Open a repository, the configuration dictionary, and the releases database */
REPO *
repo_open(const char *progname, const char *repopath, int sqliteflags, int requiredb)
{
	REPO *repo;
	const char *s, *t;
	char *p;
	const git_error *err;
	int r;

	repo = (REPO *) xalloc(sizeof(REPO));
	/* Determine the basename for progname */
	t = strrchr(progname, '/');
	if(t)
	{
		t++;
		repo->progname = xstrdup(t);
	}
	else
	{
		repo->progname = xstrdup(progname);
	}
	/* If no repository path was specified, attempt to use $GIT_DIR */
	if(!repopath)
	{
		repopath = getenv("GIT_DIR");
	}
	if(repopath)
	{
		repo->path = xstrdup(repopath);
	}
	else
	{
		/* If no path was specified, perform discovery */
		if(git_repository_discover(&(repo->pathbuf), ".", 0, "/"))
		{
			err = giterr_last();
			fprintf(stderr, "%s: %s\n", repo->progname, err->message);
			repo_close(repo);
			return NULL;
		}
		repo->path = repo->pathbuf.ptr;
	}
	/* Attempt to open the repository */
	if(git_repository_open(&(repo->repo), repo->path))
	{
		err = giterr_last();
		fprintf(stderr, "%s: %s: %s\n", repo->progname, repo->path, err->message);
		repo_close(repo);
		return NULL;
	}
	/* Determine the release database path */
	repo->dbpath = (char *) xalloc(strlen(repo->path) + 32);
	strcpy(repo->dbpath, repo->path);
	p = strchr(repo->dbpath, 0);
	if(p > repo->dbpath)
	{
		p--;
		if(*p != '/')
		{
			p++;
			*p = '/';
			p++;
			*p = 0;
		}
	}
	strcat(repo->dbpath, "releases.sqlite3");

	/* Open the configuration dictionary */
	if(git_repository_config(&(repo->cfg), repo->repo))
	{
		err = giterr_last();
		fprintf(stderr, "%s: %s: %s\n", repo->progname, repo->path, err->message);
		repo_close(repo);
		return NULL;
	}

	/* Determine the repository's name */
	if(!git_config_get_string(&t, repo->cfg, "package.name"))
	{
		repo->name = xstrdup(t);
	}
	else
	{
		repopath = repo->path;
		repo->name = (char *) xalloc(strlen(repopath) + 1);
		while(repopath)
		{
			t = strchr(repopath, '/');
			if(!t)
			{
				strcpy(repo->name, repopath);
				break;
			}
			/* Skip any duplicate slashes */
			for(s = t; *s == '/'; s++);
			/* Backtrack if the name ended with a slash or the last path component is
			 * ".git"
			 */
			if(!*s || !strcmp(s, ".git") || !strcmp(s, ".git/"))
			{
				strcpy(repo->name, repopath);
				repo->name[(t - repopath)] = 0;
				break;
			}
			repopath = t + 1;
		}
		/* Trim a .git suffix if present */
		if(strlen(repo->name) > 4)
		{
			p = strchr(repo->name, 0);
			p -= 4;
			if(!strcmp(p, ".git"))
			{
				*p = 0;
			}
		}
	}

	/* Open the releases database */
	r = sqlite3_open_v2(repo->dbpath, &(repo->db), sqliteflags, NULL);
	if(r != SQLITE_OK)
	{
		if(requiredb || r != SQLITE_CANTOPEN)
		{
			fprintf(stderr, "%s: %s: %s\n", repo->progname, repo->dbpath, sqlite3_errmsg(repo->db));
			repo_close(repo);
			return NULL;
		}
		/* The database is optional */
		sqlite3_close(repo->db);
		repo->db = NULL;
	}
	return repo;
}

/* Close a repository, freeing resources */
int
repo_close(REPO *repo)
{
	if(!repo)
	{
		errno = EINVAL;
		return -1;
	}
	if(repo->pathbuf.ptr)
	{
		git_buf_free(&(repo->pathbuf) );
	}
	else
	{
		free(repo->path);
	}
	sqlite3_close(repo->db);
	free(repo->progname);
	free(repo->dbpath);
	free(repo->name);
	free(repo);   
	return 0;
}

/* Check if a given tag name is a release tag, returning a pointer to the
 * start of the version number if so, or NULL if not
 */
const char *
check_release_tag(const char *tag_name)
{
	const char *t;

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
		tag_name += 8;
	}
	if(!tag_name[0])
	{
		return NULL;
	}
	/* Check that the remainder of the tag looks something
	 * like a version number: this means that it must consist
	 * of a string which begins with '999.9', where '999' is
	 * any number of digits.
	 */
	t = tag_name;
	while(*t && isdigit(*t))
	{
		t++;
	}
	if(*t != '.')
	{
		return NULL;
	}
	t++;
	if(!isdigit(*t))
	{
		return NULL;
	}	
	/* Check that the tag contains only characters that we consider
	 * valid.
	 */
	for(; *t; t++)
	{
		if(!isalnum(*t) && *t != '-' && *t != '_' && *t != '.' && *t != '~' && *t != '@')
		{
			return NULL;
		}
	}
	if(strlen(tag_name) > 32)
	{
		return NULL;
	}
	return tag_name;
}

/* Check the name of a branch to ensure it's something we consider valid
 * as a release-tracking branch name
 */
const char *
check_release_branch(const char *branch_name)
{
	const char *t;

	for(t = branch_name; *t; t++)
	{
		if(!isalnum(*t) && *t != '-' && *t != '_')
		{
			return NULL;
		}
	}
	if(strlen(branch_name) > 32)
	{
		return NULL;
	}
	return branch_name;
}

/* Convert a git_time to a UTC struct tm and accompanying hours/minutes
 * offset and sign
 */
int
gmgittime(const git_time *time, struct tm *tm, int *hours, int *minutes, char *signptr)
{
	int offset, sign;
	time_t t;
	
	offset = time->offset;
	t = (time_t) time->time + (offset * 60);
	sign = (offset < 0 ? '-' : '+');
	if(tm)
	{
		gmtime_r(&t, tm);
	}
	if(hours && minutes && signptr)
	{
		if(offset < 0)
		{
			offset = -offset;
		}
		*hours = offset / 60;
		*minutes = offset % 60;
		*signptr = sign;
	}
	return 0;
}
