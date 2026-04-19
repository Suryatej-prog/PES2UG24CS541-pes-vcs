#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;
    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;
        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);
    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }
    *data_out = buffer;
    *len_out = offset;
    return 0;
}

int tree_from_index(ObjectID *id_out) {
    Index *index = calloc(1, sizeof(Index));
    if (!index) return -1;

    if (index_load(index) != 0) {
        free(index);
        return -1;
    }

    if (index->count == 0) {
        free(index);
        return -1;
    }

    Tree tree;
    tree.count = 0;

    for (int i = 0; i < index->count; i++) {
        if (tree.count >= MAX_TREE_ENTRIES) break;
        TreeEntry *e = &tree.entries[tree.count];
        e->mode = index->entries[i].mode;
        e->hash = index->entries[i].hash;
        const char *path = index->entries[i].path;
        const char *slash = strchr(path, '/');
        if (slash) {
            size_t len = slash - path;
            if (len >= sizeof(e->name)) len = sizeof(e->name) - 1;
            strncpy(e->name, path, len);
            e->name[len] = '\0';
        } else {
            strncpy(e->name, path, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
        }
        tree.count++;
    }

    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) {
        free(index);
        return -1;
    }

    int rc = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    free(index);
    return rc;
}
