#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <git2.h>

struct branch_filter_struct
{
	unsigned type;
	int (*cb)(git_reference *ref, const char *branch_name, git_branch_t type, void *data);
	git_oid *oid;
	git_repository *repo;
	git_revwalk *walker;
};

static int
branch_callback(git_reference *ref, const char *ref_name, git_branch_t branch_type, void *data)
{
	char buf[GIT_OID_HEXSZ + 1];
	const char *type;
	git_oid *oid;
	git_oid tip, found;
	int match;
	struct branch_filter_struct *filter;
	git_reference *resolved;
	
	filter = (struct branch_filter_struct *) data;	
	oid = filter->oid;
	resolved = NULL;
	git_reference_resolve(&resolved, ref);
	git_oid_cpy(&tip, git_reference_target(resolved));
	git_reference_free(resolved);
	/* Determine whether oid exists in this branch's history by revwalking the
	 * history
	 */
	git_revwalk_reset(filter->walker);
	git_revwalk_push(filter->walker, &tip);
	git_revwalk_push_glob(filter->walker, ref_name);
	match = 0;
	if(!strcmp(ref_name, "refs/heads/squeeze-backports"))
	{
		fprintf(stderr, "checking ref %s\n", ref_name);
	}
	while(!git_revwalk_next(&found, filter->walker))
	{
		git_oid_tostr(buf, sizeof(buf), &found);
		if(!strcmp(ref_name, "refs/heads/squeeze-backports"))
		{
			fprintf(stderr, "  %s\n", buf);
		}
		if(!git_oid_cmp(&found, oid))
		{
			match = 1;
			break;
		}
	}
	if(!match)
	{
		return 0;
	}
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
	git_oid_tostr(buf, sizeof(buf), oid);
	printf("%s (%s) contains %s\n", ref_name, type, buf);
	return 0;
}

static int
ref_callback(git_reference *ref, void *data)
{
	struct branch_filter_struct *filter;
	const char *ref_name;
	int remote;
	
	if(!git_reference_is_branch(ref))
	{
		return 0;
	}
	remote = git_reference_is_remote(ref);
	ref_name = git_reference_name(ref);
	filter = (struct branch_filter_struct *) data;

	if(filter->type & GIT_BRANCH_LOCAL && !remote)
	{
		return filter->cb(ref, ref_name, GIT_BRANCH_LOCAL, data);
	}
	if(filter->type & GIT_BRANCH_REMOTE && remote)
	{
		return filter->cb(ref, ref_name, GIT_BRANCH_REMOTE, data);
	}
	return 0;
}

static void
usage(const char *progname)
{
	fprintf(stderr, "Usage: %s COMMIT [PATH-TO-REPO]\nHonours GIT_DIR if set.\n", progname);
}

int
main(int argc, char **argv)
{
	git_buf pathbuf;
	size_t size;
	const char *path, *commit;
	const git_error *err;
	git_repository *repo;
	struct branch_filter_struct filter;
	git_oid oid;
	
	path = NULL;
	if(argc == 3)
	{
		path = argv[2];
	}
	else if(argc != 2)
	{
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	commit = argv[1];
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
	if(git_oid_fromstr(&oid, commit))
	{
		err = giterr_last();
		fprintf(stderr, "%s: %s\n", path, err->message);
		exit(EXIT_FAILURE);		
	}
	git_revwalk_new(&(filter.walker), repo);
	filter.repo = repo;
	filter.oid = &oid;
	filter.cb = branch_callback;
	filter.type = GIT_BRANCH_LOCAL | GIT_BRANCH_REMOTE;
	git_reference_foreach(repo, ref_callback, &filter);
	git_revwalk_free(filter.walker);
	git_repository_free(repo);
	git_buf_free(&pathbuf);
	return 0;
}
