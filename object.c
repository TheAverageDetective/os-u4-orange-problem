// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

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

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
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

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // Determine type string
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob"; break;
        case OBJ_TREE:   type_str = "tree"; break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // 1. Build the header
    char header[256];
    int header_len = sprintf(header, "%s %zu", type_str, len);
    if (header_len < 0 || header_len >= (int)sizeof(header)) return -1;

    // 2. Combine header (with the null terminator) + data
    size_t full_len = header_len + 1 + len;  // +1 for the null terminator
    void *full_object = malloc(full_len);
    if (!full_object) return -1;
    
    memcpy(full_object, header, header_len);
    ((char *)full_object)[header_len] = '\0';
    if (len > 0 && data) {
        memcpy((char *)full_object + header_len + 1, data, len);
    }

    // 3. Compute SHA-256 hash
    compute_hash(full_object, full_len, id_out);

    // 4. Check for deduplication
    if (object_exists(id_out)) {
        free(full_object);
        return 0;  // Already exists, no need to write
    }

    // 5. Get the final path and create shard directory
    char path[512];
    object_path(id_out, path, sizeof(path));

    // Extract shard directory from path (everything up to the last '/')
    char shard_dir[512];
    if (strlen(path) >= sizeof(shard_dir)) {
        free(full_object);
        return -1;
    }
    strcpy(shard_dir, path);
    char *last_slash = strrchr(shard_dir, '/');
    if (!last_slash) {
        free(full_object);
        return -1;  // Path must have a slash
    }
    *last_slash = '\0';
    
    // Create shard directory if it doesn't exist
    mkdir(shard_dir, 0755);

    // 6. Write to a temporary file
    char tmp_path[528];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path)) {
        free(full_object);
        return -1;
    }

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full_object);
        return -1;
    }

    ssize_t written = write(fd, full_object, full_len);
    if (written < 0 || (size_t)written != full_len) {
        close(fd);
        unlink(tmp_path);
        free(full_object);
        return -1;
    }

    // 7. fsync() the temp file to disk
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        free(full_object);
        return -1;
    }
    close(fd);

    // 8. Atomic rename
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        free(full_object);
        return -1;
    }

    // 9. fsync() the shard directory to persist the rename
    int shard_fd = open(shard_dir, O_RDONLY);
    if (shard_fd >= 0) {
        fsync(shard_fd);
        close(shard_fd);
    }

    free(full_object);
    return 0;
}

// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // 1. Build the file path
    char path[512];
    object_path(id, path, sizeof(path));

    // 2. Open and read the entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;  // File not found

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    void *file_data = malloc(file_size);
    if (!file_data) {
        fclose(f);
        return -1;
    }

    if (fread(file_data, 1, file_size, f) != (size_t)file_size) {
        free(file_data);
        fclose(f);
        return -1;
    }
    fclose(f);

    // 3. Verify integrity by recomputing hash
    ObjectID computed_id;
    compute_hash(file_data, file_size, &computed_id);

    if (memcmp(&computed_id, id, sizeof(ObjectID)) != 0) {
        // Hash mismatch - object is corrupt
        free(file_data);
        return -1;
    }

    // 4. Parse the header
    const uint8_t *null_ptr = (const uint8_t *)memchr(file_data, '\0', file_size);
    if (!null_ptr) {
        free(file_data);
        return -1;  // No null terminator found
    }

    // Extract type string
    size_t header_len = null_ptr - (const uint8_t *)file_data;
    char header[256];
    if (header_len >= sizeof(header)) {
        free(file_data);
        return -1;
    }
    memcpy(header, file_data, header_len);
    header[header_len] = '\0';

    // Parse type and size from header
    char type_str[32];
    size_t declared_size;
    if (sscanf(header, "%31s %zu", type_str, &declared_size) != 2) {
        free(file_data);
        return -1;
    }

    // Map type string to ObjectType
    ObjectType type;
    if (strcmp(type_str, "blob") == 0) {
        type = OBJ_BLOB;
    } else if (strcmp(type_str, "tree") == 0) {
        type = OBJ_TREE;
    } else if (strcmp(type_str, "commit") == 0) {
        type = OBJ_COMMIT;
    } else {
        free(file_data);
        return -1;  // Unknown type
    }

    // 5. Extract the data portion (after the null terminator)
    const void *data_ptr = null_ptr + 1;
    size_t data_len = file_size - header_len - 1;

    // Verify the declared size matches
    if (data_len != declared_size) {
        free(file_data);
        return -1;
    }

    // 6. Allocate and return the data
    void *data_copy = malloc(data_len);
    if (!data_copy) {
        free(file_data);
        return -1;
    }
    memcpy(data_copy, data_ptr, data_len);

    *type_out = type;
    *data_out = data_copy;
    *len_out = data_len;

    free(file_data);
    return 0;
}
