#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <git2.h>

struct branch_filter_struct
{
	unsigned type;
	int (*cb)(const char *branch_name, git_branch_t type, void *data);
	void *data;
};

static int
branch_callback(const char *branch_name, git_branch_t branch_type, void *data)
{
	const char *type;
	
	(void) data;
	
	switch(branch_type)
	{
	case GIT_BRANCH_LOCAL:
		type = "local";
		break;
	case GIT_BRANCH_REMOTE:
		type = "remote";
		break;
	default:
		type = "unknown";
	}
	fprintf(stderr, "%s (%s)\n", branch_name, type);
	return 0;
}

static int
ref_callback(const char *ref_name, void *data)
{
	const char *local_prefix = "refs/heads/";
	const char *remote_prefix = "refs/remotes/";
	struct branch_filter_struct *filter;
	size_t l;
	
	filter = (struct branch_filter_struct *) data;
	
	if(filter->type & GIT_BRANCH_LOCAL)
	{
		l = strlen(local_prefix);
		if(!strncmp(ref_name, local_prefix, l))
		{
			return filter->cb(ref_name + l, GIT_BRANCH_LOCAL, filter->data);
		}
	}
	if(filter->type & GIT_BRANCH_REMOTE)
	{
		l = strlen(remote_prefix);
		if(!strncmp(ref_name, remote_prefix, l))
		{
			return filter->cb(ref_name + l, GIT_BRANCH_REMOTE, filter->data);
		}
	}
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
	char *pathbuf;
	size_t size;
	const char *path;
	const git_error *err;
	git_repository *repo;
	struct branch_filter_struct filter;
	
	if(argc == 2)
	{
		path = argv[1];
	}
	else if(argc != 1)
	{
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	if(!path)
	{
		path = getenv("GIT_DIR");
	}
	pathbuf = NULL;
	if(!path)
	{
		size = (size_t) pathconf(".", _PC_PATH_MAX);
		pathbuf = (char *) malloc(size + 1);
		if(!pathbuf)
		{
			perror(argv[0]);
			exit(EXIT_FAILURE);
		}
		if(git_repository_discover(pathbuf, size + 1, ".", 0, "/"))
		{
			err = giterr_last();
			free(pathbuf);
			fprintf(stderr, "%s: %s\n", path, err->message);
			exit(EXIT_FAILURE);
		}
		path = pathbuf;
	}
	if(git_repository_open(&repo, path))
	{
		err = giterr_last();
		fprintf(stderr, "%s: %s\n", path, err->message);
		exit(EXIT_FAILURE);
	}
	/* Only available in HEAD:
	git_branch_foreach(repo, GIT_BRANCH_LOCAL, branch_callback, NULL);
	 */
	filter.data = NULL;
	filter.cb = branch_callback;
	filter.type = GIT_BRANCH_LOCAL | GIT_BRANCH_REMOTE;
	git_reference_foreach(repo, GIT_REF_LISTALL, ref_callback, &filter);
	git_repository_free(repo);
	free(pathbuf);
	return 0;
}