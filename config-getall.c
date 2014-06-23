#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <git2.h>

static int
config_callback(const git_config_entry *entry, void *data)
{
	(void) data;
	
	puts(entry->value);
	return 0;
}

static void
usage(const char *progname)
{
	fprintf(stderr, "Usage: %s VAR [PATH-TO-REPO]\nHonours GIT_DIR if set.\n", progname);
}

int
main(int argc, char **argv)
{
	git_buf pathbuf;
	const char *path = NULL;
	const git_error *err;
	git_repository *repo;
	git_config *cfg;
	
	if(argc == 3)
	{
		path = argv[2];
	}
	else if(argc != 2)
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
	if(git_repository_config(&cfg, repo))
	{
		err = giterr_last();
		fprintf(stderr, "%s: %s\n", path, err->message);
		exit(EXIT_FAILURE);		
	}
	git_config_get_multivar_foreach(cfg, argv[1], NULL, config_callback, NULL);
	git_config_free(cfg);
	git_repository_free(repo);
	git_buf_free(&pathbuf);
	return 0;
}
