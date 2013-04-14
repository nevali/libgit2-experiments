#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <git2.h>

struct tag_filter_struct
{
	int (*cb)(const char *tag_name, git_oid *oid, void *data);
	void *data;
};

static int
tag_callback(const char *tag_name, git_oid *oid, void *data)
{
	(void) oid;
	(void) data;
	
	fprintf(stderr, "%s\n", tag_name);
	return 0;
}

static int
ref_callback(const char *ref_name, void *data)
{
	const char *prefix = "refs/tags/";
	struct tag_filter_struct *filter;
	size_t l;
	
	filter = (struct tag_filter_struct *) data;
	
	l = strlen(prefix);
	if(!strncmp(ref_name, prefix, l))
	{
		return filter->cb(ref_name + l, NULL, filter->data);
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
	const char *path;
	const git_error *err;
	git_repository *repo;
	struct tag_filter_struct filter;
	
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
	/* XXX use git_repository_discover */
	if(!path)
	{
		path = ".";
	}
	if(git_repository_open(&repo, path))
	{
		err = giterr_last();
		fprintf(stderr, "%s: %s\n", path, err->message);
		exit(EXIT_FAILURE);
	}
	/* Only available in HEAD:
	git_tag_foreach(repo, tag_callback, NULL);
	 */
	filter.data = NULL;
	filter.cb = tag_callback;
	git_reference_foreach(repo, GIT_REF_LISTALL, ref_callback, &filter);
	git_repository_free(repo);
	return 0;
}