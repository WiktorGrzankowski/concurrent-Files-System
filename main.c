#include "HashMap.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>



int main(void)
{
	Tree *tree = tree_new();
	char *list_content = tree_list(tree, "/");
	assert(strcmp(list_content, "") == 0);
	free(list_content);
	assert(tree_list(tree, "/a/") == NULL);
	assert(tree_create(tree, "/a/") == 0);
	assert(tree_create(tree, "/a/b/") == 0);
	assert(tree_create(tree, "/a/b/") == EEXIST);
	assert(tree_create(tree, "/a/b/c/d/") == ENOENT);
	assert(tree_remove(tree, "/a/") == ENOTEMPTY);
	assert(tree_create(tree, "/b/") == 0);
	assert(tree_create(tree, "/a/c/") == 0);
	assert(tree_create(tree, "/a/c/d/") == 0);
	assert(tree_move(tree, "/a/c/", "/b/c/") == 0);
	assert(tree_remove(tree, "/b/c/d/") == 0);
	list_content = tree_list(tree, "/b/");
	assert(strcmp(list_content, "c") == 0);
	free(list_content);
	tree_free(tree);
    return 0;
}
