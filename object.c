// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTATION ──────────────────────────────────────────────────────────
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    if (!id_out) return -1;

    const char *type_str;
    if (type == OBJ_BLOB) type_str = "blob";
    else if (type == OBJ_TREE) type_str = "tree";
    else if (type == OBJ_COMMIT) type_str = "commit";
    else return -1;

    // Build header
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    size_t total_size = header_len + len;

    // 🔥 SAFE allocation
    char *buffer = malloc(total_size);
    if (!buffer) return -1;

    memcpy(buffer, header, header_len);

    if (len > 0 && data)
        memcpy(buffer + header_len, data, len);

    // 🔥 CRITICAL: zero id before hashing
    memset(id_out, 0, sizeof(ObjectID));

    // Compute hash
    compute_hash(buffer, total_size, id_out);

    // Deduplication
    if (object_exists(id_out)) {
        free(buffer);
        return 0;
    }

    // Build path
    char path[512];
    object_path(id_out, path, sizeof(path));

    // 🔥 SAFE directory creation
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);

    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }

    // Write file
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(buffer);
        return -1;
    }

    ssize_t written = write(fd, buffer, total_size);
    if (written != (ssize_t)total_size) {
        close(fd);
        free(buffer);
        return -1;
    }

    fsync(fd);
    close(fd);
    free(buffer);

    return 0;
}
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    char *buffer = malloc(file_size);
    if (!buffer) {
        fclose(f);
        return -1;
    }

    if (fread(buffer, 1, file_size, f) != (size_t)file_size) {
        fclose(f);
        free(buffer);
        return -1;
    }
    fclose(f);

    // Verify hash
    ObjectID computed;
    compute_hash(buffer, file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1;
    }

    // Parse header
    char *null_pos = memchr(buffer, '\0', file_size);
    if (!null_pos) {
        free(buffer);
        return -1;
    }

    *null_pos = '\0';

    char type_str[10];
    size_t size;
    if (sscanf(buffer, "%s %zu", type_str, &size) != 2) {
        free(buffer);
        return -1;
    }

    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(buffer);
        return -1;
    }

    void *data = malloc(size);
    if (!data) {
        free(buffer);
        return -1;
    }

    memcpy(data, null_pos + 1, size);

    *data_out = data;
    *len_out = size;

    free(buffer);
    return 0;
}
