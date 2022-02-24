#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "Tree.h"
#include "path_utils.h"
#include "err.h"

// Error of trying to move a folder into it's own subtree.
// For example moving /a/ to /a/b/c/, when /a/b/ exists.
#define EILLEGALMOVE -1

struct Tree {
    HashMap *children;

    pthread_mutex_t lock;
    pthread_cond_t reader_type;
    pthread_cond_t writer_type;

    int reader_type_count, writer_type_count;
    int reader_type_waiting, writer_type_waiting;
    int change;
};

void tree_reader_type_entry_protocol(Tree *tree_node) {
    if (pthread_mutex_lock(&tree_node->lock) != 0)
        syserr("lock failed");

    while (tree_node->writer_type_count + tree_node->writer_type_waiting > 0 &&
           tree_node->change <= 0) {
        tree_node->reader_type_waiting++;
        if (pthread_cond_wait(&tree_node->reader_type, &tree_node->lock) != 0)
            syserr("condition wait failed");
        tree_node->reader_type_waiting--;
    }
    tree_node->change--;
    tree_node->reader_type_count++;
    if (tree_node->change > 0) {
        if (pthread_cond_signal(&tree_node->reader_type) != 0)
            syserr("condition signal failed");
    }
    if (tree_node->change < 0)
        tree_node->change = 0;

    if (pthread_mutex_unlock(&tree_node->lock) != 0)
        syserr("mutex unlock failed");
}

void tree_reader_type_final_protocol(Tree *tree_node) {
    if (pthread_mutex_lock(&tree_node->lock) != 0)
        syserr("lock failed");
    tree_node->reader_type_count--;
    if (tree_node->reader_type_count == 0 &&
        tree_node->writer_type_waiting > 0) {
        tree_node->change = -1;
        if (pthread_cond_signal(&tree_node->writer_type) != 0)
            syserr("condition signal failed");
    }
    if (pthread_mutex_unlock(&tree_node->lock) != 0)
        syserr("mutex unlock failed");
}

void tree_writer_type_entry_protocol(Tree *tree_node) {
    if (pthread_mutex_lock(&tree_node->lock) != 0)
        syserr("lock failed");
    while (tree_node->writer_type_count + tree_node->reader_type_count > 0 &&
           tree_node->change != -1) {
        tree_node->writer_type_waiting++;

        if (pthread_cond_wait(&tree_node->writer_type, &tree_node->lock) != 0)
            syserr("condition wait failed");

        tree_node->writer_type_waiting--;
    }
    tree_node->writer_type_count++;
    tree_node->change = 0;
    if (pthread_mutex_unlock(&tree_node->lock) != 0)
        syserr("mutex unlock failed");
}

void tree_writer_type_final_protocol(Tree *tree_node) {
    if (pthread_mutex_lock(&tree_node->lock) != 0)
        syserr("lock failed");
    tree_node->writer_type_count--;

    if (tree_node->reader_type_waiting > 0) {
        tree_node->change = tree_node->reader_type_waiting;
        if (pthread_cond_signal(&tree_node->reader_type) != 0)
            syserr("condition signal failed");
    } else if (tree_node->writer_type_waiting > 0) {
        tree_node->change = -1;
        if (pthread_cond_signal(&tree_node->writer_type) != 0)
            syserr("condition signal failed");
    } else {
        tree_node->change = 0;
    }
    if (pthread_mutex_unlock(&tree_node->lock) != 0)
        syserr("mutex unlock failed");
}

Tree *tree_new() {
    Tree *tree = malloc(sizeof(Tree));
    if (!tree)
        fatal("Malloc failure.");
    tree->children = hmap_new();
    tree->change = 0;
    tree->reader_type_count = 0;
    tree->reader_type_waiting = 0;
    tree->writer_type_count = 0;
    tree->writer_type_waiting = 0;
    if (pthread_mutex_init(&tree->lock, 0) != 0)
        syserr("mutex init failed");
    if (pthread_cond_init(&tree->reader_type, 0) != 0)
        syserr("cond init 1 failed");
    if (pthread_cond_init(&tree->writer_type, 0) != 0)
        syserr("cond init 2 failed");
    return tree;
}

void tree_free(Tree *tree) {
    if (!tree)
        return;

    if (hmap_size(tree->children) == 0) {
        hmap_free(tree->children);
        if (pthread_cond_destroy(&tree->reader_type) != 0)
            syserr("cond destroy 1 failed");
        if (pthread_cond_destroy(&tree->writer_type) != 0)
            syserr("cond destroy 2 failed");
        if (pthread_mutex_destroy(&tree->lock) != 0)
            syserr("mutex destroy failed");
        free(tree);
        return;
    }

    Tree *curr_tree = tree;
    const char *key;
    void *value;
    HashMapIterator it = hmap_iterator(tree->children);
    while (hmap_next(tree->children, &it, &key, &value)) {
        curr_tree = hmap_get(curr_tree->children, key);
        tree_free(curr_tree);
        curr_tree = tree;
    }
    hmap_free(tree->children);
    if (pthread_cond_destroy(&tree->reader_type) != 0)
        syserr("cond destroy 1 failed");
    if (pthread_cond_destroy(&tree->writer_type) != 0)
        syserr("cond destroy 2 failed");
    if (pthread_mutex_destroy(&tree->lock) != 0)
        syserr("mutex destroy failed");
    free(tree);
}

// Jako czytelnicy przechodzimy po kolejnych folderach na drodze do rodzica
// powstającego foldera. Rodzic w path jest pisarzem. W pętli, po przejściu
// do syna wywoływany jest protokół końcowy rodzica.
// Jeśli po drodze okaże się, że folder nie istnieje, zwracany jest
// stosowny błąd.
int tree_create(Tree *tree, const char *path) {
    if (strcmp(path, "") == 0 || !is_path_valid(path))
        return EINVAL;
    if (strcmp(path, "/") == 0)
        return EEXIST;

    char *n_path = malloc(MAX_FOLDER_NAME_LENGTH + 1);
    if (!n_path)
        fatal("Malloc failure");

    char *path_to_parent = make_path_to_parent(path, n_path);
    free(path_to_parent);
    char component[MAX_FOLDER_NAME_LENGTH + 1];
    Tree *curr_tree = tree; // zaczynamy w korzeniu
    Tree *prev_tree = NULL; // najpierw nic nie ma wczesniej
    char *subpath_mall = make_path_to_parent(path, component);
    const char *subpath = subpath_mall;

    if (!curr_tree) {
        free(subpath_mall);
        return ENOENT;
    }
    if (strcmp(subpath, "/")) { // działamy nie tylko w korzeniu
        tree_reader_type_entry_protocol(curr_tree);
    } else {
        tree_writer_type_entry_protocol(curr_tree);
    }
    while ((subpath = split_path(subpath, component))) {
        prev_tree = curr_tree;
        curr_tree = hmap_get(curr_tree->children, component);
        if (!curr_tree) {
            // nie znalezlismy folderu, konczymy protokoly
            tree_reader_type_final_protocol(prev_tree);
            free(subpath_mall);
            free(n_path);
            return ENOENT;
        }
        if (strcmp(subpath, "/")) { // jeszcze nie ostatni folder
            tree_reader_type_entry_protocol(curr_tree);
        } else { // doszliśmy do końca, jesteśmy pisarzem
            tree_writer_type_entry_protocol(curr_tree);
        }
        tree_reader_type_final_protocol(prev_tree);
    }
    Tree *n_tree = tree_new();
    bool insert_success = hmap_insert(curr_tree->children, n_path, n_tree);

    if (!insert_success) { // nie udało się wstawić, taki syn już istnieje
        tree_free(n_tree);
        tree_writer_type_final_protocol(curr_tree);
        free(subpath_mall);
        free(n_path);
        return EEXIST;
    }
    tree_writer_type_final_protocol(curr_tree);
    free(subpath_mall);
    free(n_path);
    return 0;
}

// Przechodzimy po kolejnych folderach w scieżce path jako czytelnicy.
// W docelowym folderze jest wykonywana czynność czytelnika.
// Jeśli po drodze okaże się, że folderu nie ma, po wywołaniu protokołu
// końcowego rodzica, zwracany jest NULL.
char *tree_list(Tree *tree, const char *path) {
    if (!is_path_valid(path))
        return NULL;

    char component[MAX_FOLDER_NAME_LENGTH + 1];
    Tree *curr_tree = tree;
    Tree *prev_tree = NULL;

    const char *subpath = path;
    if (!curr_tree)
        return NULL;

    tree_reader_type_entry_protocol(curr_tree); // zaczynamy czytać w korzeniu
    while ((subpath = split_path(subpath, component))) {
        prev_tree = curr_tree;
        curr_tree = hmap_get(curr_tree->children, component);
        if (!curr_tree) {
            tree_reader_type_final_protocol(prev_tree);
            return NULL;
        }
        tree_reader_type_entry_protocol(curr_tree);
        tree_reader_type_final_protocol(prev_tree);
    }
    // doszliśmy do folderu, pobieramy jego zawartość
    char *list = make_map_contents_string(curr_tree->children);
    tree_reader_type_final_protocol(curr_tree);
    return list;
}

// Przechodzimy do rodzica docelowo usuwanego folderu jako czytelnicy.
// Rodzic ostatniego folderu na scieżce path działa jako pisarz
// i usuwa z listy swoich dzieci podany folder.
// Jeśli gdzieś po drodze okaże się, że jakiś folder nie istnieje,
// zwalniane jest "miejsce w bibliotece" i zwracany stosowny błąd.
int tree_remove(Tree *tree, const char *path) {
    if (!is_path_valid(path))
        return EINVAL;
    if (strcmp(path, "/") == 0)
        return EBUSY;

    char component[MAX_FOLDER_NAME_LENGTH + 1];
    char componentToRemove[MAX_FOLDER_NAME_LENGTH + 1];
    Tree *curr_tree = tree; // zaczynamy w korzeniu
    Tree *prev_tree = NULL; // najpierw nic nie ma wcześniej
    char *subpath_mall = make_path_to_parent(path, componentToRemove);
    const char *subpath = subpath_mall;
    if (!curr_tree) {
        free(subpath_mall);
        return ENOENT;
    }
    if (strcmp(subpath, "/")) { // nie tylko w korzeniu działamy
        // zaczynamy czytać w korzeniu
        tree_reader_type_entry_protocol(curr_tree);
    } else { // od razu piszemy w korzeniu, nie wchodzimy do pętli
        tree_writer_type_entry_protocol(curr_tree);
    }
    // chodzenie po drzewie aż do podanego folderu
    while ((subpath = split_path(subpath, component))) {
        prev_tree = curr_tree;
        curr_tree = hmap_get(curr_tree->children, component);
        if (!curr_tree) {
            tree_reader_type_final_protocol(prev_tree);
            free(subpath_mall);
            return ENOENT;
        }
        if (strcmp(subpath, "/")) { // jeszcze nie ostatni folder
            tree_reader_type_entry_protocol(curr_tree);
        } else { // doszliśmy do końca, jesteśmy pisarzem
            tree_writer_type_entry_protocol(curr_tree);
        }
        tree_reader_type_final_protocol(prev_tree);
    }

    free(subpath_mall);
    // węzeł drzewa do usunięcia
    Tree *final_tree = hmap_get(curr_tree->children, componentToRemove);
    if (!final_tree) {
        tree_writer_type_final_protocol(curr_tree);
        return ENOENT;
    }
    tree_reader_type_final_protocol(final_tree);
    if (hmap_size(final_tree->children) != 0) {
        tree_writer_type_final_protocol(curr_tree);
        tree_reader_type_final_protocol(final_tree);
        return ENOTEMPTY;
    }
    hmap_remove(curr_tree->children, componentToRemove);
    tree_free(final_tree);
    tree_writer_type_final_protocol(curr_tree);
    return 0;
}

// Sprawdza, czy target jest podfolderem source.
bool moving_to_own_subtree(const char *source, const char *target) {
    size_t len_shorter;
    if (strlen(source) < strlen(target))
        len_shorter = strlen(source);
    else
        return false; // jak target jest krótszy, nie może być podfolderem
    for (size_t i = 0; i < len_shorter; ++i) {
        if (source[i] != target[i])
            return false;
    }
    // target jest dłuższy lub równy, i na całej długości source są takie same
    return true;
}

// Zwraca wspólną ścieżkę podanych dwóch scieżek.
char *shared_path(const char *to_source, const char *to_target) {
    char *subpath = strchr(to_source + 1, '/'); // + 1 by uniknąć pierwszego '/'

    size_t total_length = 1;
    while (subpath) {
        size_t curr_length = subpath - (to_source + total_length) + 1;
        if (strncmp(to_source + total_length, to_target + total_length,
                    curr_length) != 0) {
            subpath = NULL;
        } else {
            total_length += curr_length;
            subpath = strchr(subpath + 1, '/');
        }
    }
    total_length++; // na końcowe \0

    char *shared = malloc(total_length);
    if (!shared)
        fatal("Malloc failure.");

    strncpy(shared, to_source, total_length - 1);
    shared[total_length - 1] = '\0';
    return shared;
}

// Przechodzimy jako czytelnicy do LCA rodziców source i target.
// Blokujemy poddrzewo wywołując protokół wstępny pisarza na LCA.
// Następnie jako czytelnicy dochodzimy do rodzica source, gdzie
// wykonywane są operacje pisarza. Potem analogicznie dla rodzica target.
// Folder LCA zwalniamy jest po zakończeniu całej operacji.
// Jeśli po drodze okaże się, że jakiś folder nie istnieje,
// wywoływane są protokoły końcowe i zwracany jest stosowny błąd.
int tree_move(Tree *tree, const char *source, const char *target) {
    if (!is_path_valid(source) || !is_path_valid(target))
        return EINVAL;
    if (strcmp(source, "/") == 0)
        return EBUSY;
    if (strcmp(target, "/") == 0)
        return EEXIST;
    if (moving_to_own_subtree(source, target))
        return EILLEGALMOVE;

    char comp_target[MAX_FOLDER_NAME_LENGTH + 1];
    char *help = make_path_to_parent(target, comp_target);
    char comp_source[MAX_FOLDER_NAME_LENGTH + 1];
    char *help2 = make_path_to_parent(source, comp_source);
    free(help);
    free(help2);

    char component[MAX_FOLDER_NAME_LENGTH + 1];
    char *path_to_parent = make_path_to_parent(target, component);
    char *shared = shared_path(source, path_to_parent);
    char comp_src_help[MAX_FOLDER_NAME_LENGTH + 1];
    char comp_tgt_help[MAX_FOLDER_NAME_LENGTH + 1];

    Tree *curr_tree = tree;
    Tree *prev_tree = NULL;
    if (strcmp(shared, "/") == 0) { // działamy nie tylko w korzeniu
        tree_writer_type_entry_protocol(curr_tree);
    } else {
        tree_reader_type_entry_protocol(curr_tree);
        const char *subpath_source = source;
        const char *subpath_target_parent = path_to_parent;
        subpath_source = split_path(subpath_source, comp_src_help);
        subpath_target_parent = split_path(subpath_target_parent,
                                           comp_tgt_help);
        // chodzenie po drzewie aż do LCA włącznie, który zostaje pisarzem
        while (strcmp(comp_src_help, comp_tgt_help) == 0) {
            prev_tree = curr_tree;
            curr_tree = hmap_get(curr_tree->children, comp_src_help);
            if (!curr_tree) {
                tree_reader_type_final_protocol(prev_tree);
                free(path_to_parent);
                free(shared);
                return ENOENT;
            }
            subpath_source = split_path(subpath_source, comp_src_help);
            subpath_target_parent = split_path(subpath_target_parent,
                                               comp_tgt_help);

            if (strcmp(comp_src_help, comp_tgt_help) ||
                !subpath_target_parent) {
                tree_writer_type_entry_protocol(curr_tree);
                tree_reader_type_final_protocol(prev_tree);
                break;
            }
            tree_reader_type_entry_protocol(curr_tree);
            tree_reader_type_final_protocol(prev_tree);
        }
    }

    const char *path_target_parent_left = path_to_parent + strlen(shared) - 1;
    char *path_to_parent_src = make_path_to_parent(source, component);
    const char *path_source_parent_left =
            path_to_parent_src + strlen(shared) - 1;

    Tree *source_tree = curr_tree;
    Tree *target_tree = curr_tree;

    if (!curr_tree) {
        free(path_to_parent);
        free(path_to_parent_src);
        free(shared);
        return ENOENT;
    }

    bool first = true;
    // chodzenie po drzewie do rodzica source
    while ((path_source_parent_left = split_path(path_source_parent_left,
                                                 component))) {
        prev_tree = source_tree;
        source_tree = hmap_get(source_tree->children, component);
        if (!source_tree) {
            if (prev_tree->writer_type_count > 0) {
                tree_writer_type_final_protocol(prev_tree);
            } else {
                tree_reader_type_final_protocol(prev_tree);
            }
            if (prev_tree != curr_tree)
                tree_writer_type_final_protocol(curr_tree); // pisanie w LCA
            free(path_to_parent);
            free(path_to_parent_src);
            free(shared);
            return ENOENT;
        }
        if (strcmp(path_source_parent_left, "/") != 0) { // jeszcze nie ostatni
            tree_reader_type_entry_protocol(source_tree);
        } else { // doszliśmy do końca, jesteśmy pisarzem
            tree_writer_type_entry_protocol(source_tree);
        }
        if (!first)
            tree_reader_type_final_protocol(prev_tree);
        first = false;
    }
    Tree *source_to_remove = hmap_get(source_tree->children, comp_source);
    if (!source_to_remove) {
        if (!first)
            tree_writer_type_final_protocol(source_tree);
        tree_writer_type_final_protocol(curr_tree);
        free(path_to_parent);
        free(path_to_parent_src);
        free(shared);
        return ENOENT;
    }
    // dzieci source, potem potrzebne do wstawienia do folderu target
    HashMap *copy_of_source = source_to_remove->children;
    bool first_target = true;
    // chodzenie po drzewie aż do rodzica targetu
    while ((path_target_parent_left = split_path(path_target_parent_left,
                                                component))) {
        prev_tree = target_tree;
        target_tree = hmap_get(target_tree->children, component);
        if (!target_tree) {
            // nie znalezlismy folderu, konczymy protokoly
            if (prev_tree != curr_tree)
                tree_reader_type_final_protocol(prev_tree);
            if (source_tree != curr_tree)
                tree_writer_type_final_protocol(source_tree);
            tree_writer_type_final_protocol(curr_tree);
            free(path_to_parent);
            free(path_to_parent_src);
            free(shared);
            return ENOENT;
        }
        if (strcmp(path_target_parent_left, "/")) { // jeszcze nie ostatni
            tree_reader_type_entry_protocol(target_tree);
        } else { // doszliśmy do końca, jesteśmy pisarzem
            tree_writer_type_entry_protocol(target_tree);
        }

        if (!first_target && prev_tree != curr_tree) {
            if (prev_tree->writer_type_count == 0)
                tree_reader_type_final_protocol(prev_tree);
            else
                tree_writer_type_final_protocol(prev_tree);
        }
        first_target = false;
    }
    Tree *n_tree = tree_new();
    hmap_free(n_tree->children);
    n_tree->children = copy_of_source;
    bool insert_success = hmap_insert(target_tree->children, comp_target,
                                      n_tree);
    if (!insert_success) { // nie udało się wstawić, taki syn już istnieje
        free(path_to_parent_src);
        free(path_to_parent);
        free(shared);
        free(n_tree);
        if (!first)
            tree_writer_type_final_protocol(source_tree);
        if (!first_target)
            tree_writer_type_final_protocol(target_tree);
        tree_writer_type_final_protocol(curr_tree);
        return EEXIST;
    }

    free(source_to_remove);
    hmap_remove(source_tree->children, comp_source);
    if (!first)
        tree_writer_type_final_protocol(source_tree);
    if (!first_target)
        tree_writer_type_final_protocol(target_tree);
    tree_writer_type_final_protocol(curr_tree);

    free(path_to_parent_src);
    free(path_to_parent);
    free(shared);

    return 0;
}