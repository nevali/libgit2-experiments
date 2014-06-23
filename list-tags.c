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
ref_callback(git_reference *ref, void *data)
{
	struct tag_filter_struct *filter;
	git_reference *resolved;
	git_oid oid;
	
	if(!git_reference_is_tag(ref))
	{
		return 0;
	}
	filter = (struct tag_filter_struct *) data;
	resolved = NULL;
	git_reference_resolve(&resolved, ref);
	git_oid_cpy(&oid, git_reference_target(resolved));
	git_reference_free(resolved);
	return filter->cb(git_reference_name(ref), &oid, filter->data);
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
	struct tag_filter_struct filter;
	
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
	git_tag_foreach(repo, tag_callback, NULL);
	 */
	filter.data = NULL;
	filter.cb = tag_callback;
	filter.repo = repo;
	git_reference_foreach(repo, ref_callback, &filter);
	git_repository_free(repo);
	git_buf_free(&pathbuf);
	return 0;
}
