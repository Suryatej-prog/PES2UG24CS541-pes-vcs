#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");
    return 0;
}

int index_load(Index *index) {
    if (!index) return -1;
    memset(index, 0, sizeof(Index));
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        char hex[HASH_HEX_SIZE + 1];
        unsigned int mode;
        uint64_t mtime;
        uint32_t size;
        char path[512];
        if (fscanf(f, "%o %64s %" SCNu64 " %u %511s",
                   &mode, hex, &mtime, &size, path) != 5)
            break;
        e->mode      = mode;
        e->mtime_sec = mtime;
        e->size      = size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        if (hex_to_hash(hex, &e->hash) != 0) break;
        index->count++;
    }
    fclose(f);
    return 0;
}

static int cmp_index(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    if (!index) return -1;
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) { fclose(f); return -1; }
    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), cmp_index);
    for (int i = 0; i < sorted->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted->entries[i].hash, hex);
        fprintf(f, "%o %s %" PRIu64 " %u %s\n",
                sorted->entries[i].mode,
                hex,
                sorted->entries[i].mtime_sec,
                sorted->entries[i].size,
                sorted->entries[i].path);
    }
    free(sorted);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    if (!index || !path) return -1;
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "error: cannot stat '%s'\n", path);
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    void *buf = malloc(fsz > 0 ? fsz : 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, fsz, f);
    fclose(f);
    ObjectID id;
    if (object_write(OBJ_BLOB, buf, nread, &id) != 0) {
        free(buf);
        return -1;
    }
    free(buf);
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        existing->hash      = id;
        existing->mode      = 100644;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size      = (uint32_t)st.st_size;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        IndexEntry *e = &index->entries[index->count++];
        memset(e, 0, sizeof(IndexEntry));
        e->hash      = id;
        e->mode      = 100644;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size      = (uint32_t)st.st_size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }
    return index_save(index);
}
