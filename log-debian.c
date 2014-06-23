#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include <git2.h>

/* Output a changelog in Debian format:

package (version) branch; urgency=low

  * Log entry.
  * Log entry.
  * Log entry.

 -- Name <email@address>  Day, DD Mon Year HH:MM:SS +ZZZZ

*/

struct repo_data_struct
{
	git_repository *repo;
	const char *path;
	char *name;
};

struct tag_match_struct
{
	const git_oid *oid;
	char *buf;
	size_t buflen;
};


static void
usage(const char *progname)
{
	fprintf(stderr, "Usage: %s BRANCH [PATH-TO-REPO]\nHonours GIT_DIR if set.\n", progname);
}

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
tag_callback(const char *tag_name, git_oid *oid, void *data)
{
	struct tag_match_struct *match;
	const char *t;
	
	match = (struct tag_match_struct *) data;
	if(!git_oid_cmp(oid, match->oid))
	{
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
		strncpy(match->buf, tag_name, match->buflen);
		match->buf[match->buflen - 1] = 0;
		return 1;
	}
	return 0;
}

static const char *
commit_is_release(struct repo_data_struct *repo, git_commit *commit)
{
	static char version[64];
	const git_oid *id;
	struct tag_match_struct match;
	
	if(!commit)
	{
		return NULL;
	}
	id = git_commit_id(commit);
	match.oid = id;
	version[0] = 0;
	match.buf = version;
	match.buflen = sizeof(version);
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
log_commit(struct repo_data_struct *repo, git_commit *commit, const char *branchname)
{
	static const git_signature *relsig;
	const git_signature *sig;
	struct tm tm;
	int hours, minutes;
	char sign, datebuf[64];
	const char *vers;
	
	vers = commit_is_release(repo, commit);
	if(!commit || vers)
	{
		if(relsig)
		{
			git_gmtime(&(relsig->when), &tm, &hours, &minutes, &sign);
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
	sig = git_commit_author(commit);
	if(!relsig)
	{
		relsig = sig;
		printf("%s (%s) %s; urgency=low\n\n", repo->name, vers, branchname);
	}
	log_commit_message(git_commit_message(commit));
	return 0;
}

int
main(int argc, char **argv)
{
	git_buf pathbuf;
	char *namebuf, *p;
	size_t size;
	const char *path, *branch, *branchname, *s, *t;
	const git_error *err;
	const git_oid *tip;
	git_oid oid;
	git_reference *ref;
	git_revwalk *walker;
	git_commit *commit;
	struct repo_data_struct repo;
	
	if(argc < 2)
	{
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	branch = argv[1];
	path = NULL;
	if(argc == 3)
	{
		path = argv[2];
	}
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
	if(git_repository_open(&(repo.repo), path))
	{
		err = giterr_last();
		fprintf(stderr, "%s: %s\n", path, err->message);
		exit(EXIT_FAILURE);
	}
	repo.path = path;
	namebuf = (char *) malloc(strlen(path) + 1);
	repo.name = namebuf;
	while(path)
	{
		t = strchr(path, '/');
		if(!t)
		{
			strcpy(namebuf, path);
			break;
		}
		/* Skip any duplicate slashes */
		for(s = t; *s == '/'; s++);
		/* Backtrack if the name ended with a slash or the last path component is
		 * ".git"
		 */
		if(!*s || !strcmp(s, ".git") || !strcmp(s, ".git/"))
		{
			strcpy(namebuf, path);
			namebuf[(t - path)] = 0;
			break;
		}
		path = t + 1;
	}
	/* Trim a .git suffix if present */
	if(strlen(namebuf) > 4)
	{
		p = strchr(namebuf, 0);
		p -= 4;
		if(!strcmp(p, ".git"))
		{
			*p = 0;
		}
	}
	if(git_branch_lookup(&ref, repo.repo, branch, GIT_BRANCH_LOCAL))
	{
		err = giterr_last();
		fprintf(stderr, "%s: %s\n", repo.path, err->message);
		exit(EXIT_FAILURE);	
	}
	/* Obtain the canonical branch name */
	git_branch_name(&branchname, ref);
	/* Find the tip of the branch */
	tip = git_reference_target(ref);
	git_oid_cpy(&oid, tip);
	/* Create a walker for the log entries for this branch */
	git_revwalk_new(&walker, repo.repo);
	git_revwalk_sorting(walker, GIT_SORT_TOPOLOGICAL);
	git_revwalk_push(walker, &oid);
	while(!git_revwalk_next(&oid, walker))
	{
		if(git_commit_lookup(&commit, repo.repo, &oid))
		{
			err = giterr_last();
			fprintf(stderr, "%s: %s\n", repo.path, err->message);
			exit(EXIT_FAILURE);	
		}
		log_commit(&repo, commit, branchname);
	}
	log_commit(&repo, NULL, NULL);
	git_revwalk_free(walker);
	git_repository_free(repo.repo);
	git_buf_free(&pathbuf);
	return 0;
}
