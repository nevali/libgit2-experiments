#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>

#include <git2.h>

struct tag_filter_struct
{
	int (*cb)(const char *tag_name, git_oid *oid, void *data);
	void *data;
	git_repository *repo;
};

static int
tag_callback(const char *tag_name, git_oid *oid, void *data)
{
	char buf[GIT_OID_HEXSZ + 1];
	
	(void) data;
	
	git_oid_tostr(buf, sizeof(buf), oid);
	printf("%s -> %s\n", tag_name, buf);
	return 0;
}

static int
ref_callback(const char *ref_name, void *data)
{
	const char *prefix = "refs/tags/";
	struct tag_filter_struct *filter;
	size_t l;
	int r;
	git_reference *ref, *resolved;
	git_oid oid;
	
	filter = (struct tag_filter_struct *) data;
	
	l = strlen(prefix);
	if(!strncmp(ref_name, prefix, l))
	{
		ref = NULL;
		resolved = NULL;
		r = git_reference_lookup(&ref, filter->repo, ref_name);
		if(r < 0)
		{
			return r;
		}
		git_reference_resolve(&resolved, ref);
		git_reference_free(ref);
		git_oid_cpy(&oid, git_reference_oid(resolved));
		git_reference_free(resolved);
		return filter->cb(ref_name + l, &oid, filter->data);
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
	const char *path;
	const git_error *err;
	git_repository *repo;
	struct tag_filter_struct filter;
	size_t size;
	
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
	git_tag_foreach(repo, tag_callback, NULL);
	 */
	filter.data = NULL;
	filter.cb = tag_callback;
	filter.repo = repo;
	git_reference_foreach(repo, GIT_REF_LISTALL, ref_callback, &filter);
	git_repository_free(repo);
	free(pathbuf);
	return 0;
}