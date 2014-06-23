#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <git2.h>

struct branch_filter_struct
{
	unsigned type;
	int (*cb)(git_reference *ref, const char *branch_name, git_branch_t type, void *data);
	void *data;
};

static int
branch_callback(git_reference *ref, const char *branch_name, git_branch_t branch_type, void *data)
{
	const char *type;
	
	(void) ref;
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
	printf("%s (%s)\n", branch_name, type);
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
	git_repository *repo;
	git_branch_iterator *iter;
	git_branch_t type;
	git_reference *ref;
	
	struct branch_filter_struct filter;

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
	git_branch_iterator_new(&iter, repo, filter.type);
	while(git_branch_next(&ref, &type, iter) == 0)
	{
		filter.cb(ref, git_reference_name(ref), type, filter.data);
	}
	git_branch_iterator_free(iter);
	git_repository_free(repo);
	git_buf_free(&pathbuf);
	return 0;
}
