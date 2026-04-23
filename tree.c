// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Forward declarations for functions from other modules
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int index_load(struct Index *index);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.

typedef struct {
    uint32_t mode;
    ObjectID hash;
    char path[512];
} SimpleEntry;

// Helper function to recursively build tree objects
// entries: pointer to the FULL entries array
// indices: array of indices to process at this level
// count: number of indices to process
// prefix_len: length of the current directory prefix (0 for root)
// Returns 0 on success, sets *id_out to the tree hash
static int write_tree_level_helper(SimpleEntry *full_entries, int *indices, 
                                   int count, size_t prefix_len, ObjectID *id_out) {
    if (count <= 0) return -1;

    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count && tree.count < MAX_TREE_ENTRIES) {
        SimpleEntry *entry = &full_entries[indices[i]];
        const char *remaining = entry->path + prefix_len;
        const char *slash = strchr(remaining, '/');

        if (slash == NULL) {
            // This is a file at this level
            strcpy(tree.entries[tree.count].name, remaining);
            tree.entries[tree.count].mode = entry->mode;
            tree.entries[tree.count].hash = entry->hash;
            tree.count++;
            i++;
        } else {
            // This is a directory - collect all direct children
            size_t dir_name_len = slash - remaining;
            char dir_name[256];
            strncpy(dir_name, remaining, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Find all entries that belong to this directory
            int *subdir_indices = malloc(sizeof(int) * count);
            int subdir_count = 0;
            
            for (int j = i; j < count; j++) {
                SimpleEntry *check_entry = &full_entries[indices[j]];
                const char *check = check_entry->path + prefix_len;
                if (strncmp(check, dir_name, dir_name_len) == 0 &&
                    check[dir_name_len] == '/') {
                    subdir_indices[subdir_count++] = indices[j];
                }
            }

            if (subdir_count > 0) {
                // Build subtree for this directory
                ObjectID subdir_id;
                int rc = write_tree_level_helper(full_entries, subdir_indices, subdir_count,
                                                prefix_len + dir_name_len + 1, &subdir_id);
                if (rc != 0) {
                    free(subdir_indices);
                    return -1;
                }

                strcpy(tree.entries[tree.count].name, dir_name);
                tree.entries[tree.count].mode = MODE_DIR;
                tree.entries[tree.count].hash = subdir_id;
                tree.count++;

                i += subdir_count;
            } else {
                i++;
            }

            free(subdir_indices);
        }
    }

    // Serialize and store the tree
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) {
        return -1;
    }

    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    // Load from index file directly (simpler approach)
    FILE *f = fopen(".pes/index", "r");
    if (!f) {
        fprintf(stderr, "error: .pes/index not found\n");
        return -1;
    }

    SimpleEntry *entries = malloc(sizeof(SimpleEntry) * 10000);
    int *indices = malloc(sizeof(int) * 10000);
    
    if (!entries || !indices) {
        fprintf(stderr, "error: out of memory\n");
        free(entries);
        free(indices);
        fclose(f);
        return -1;
    }

    int entry_count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f) != NULL && entry_count < 10000) {
        unsigned int mode;
        char hash_hex[65];
        unsigned long mtime_sec;
        unsigned int size;
        char path[512];

        int parsed = sscanf(line, "%o %64s %lu %u %511s", &mode, hash_hex, &mtime_sec, &size, path);
        if (parsed != 5) {
            fprintf(stderr, "warning: skipping malformed index line\n");
            continue;
        }

        entries[entry_count].mode = mode;
        
        // Convert hex hash to binary
        if (strlen(hash_hex) != 64) {
            fprintf(stderr, "warning: invalid hash length\n");
            continue;
        }
        
        int valid = 1;
        for (int i = 0; i < 32; i++) {
            unsigned int byte;
            if (sscanf(hash_hex + i*2, "%2x", &byte) != 1) {
                fprintf(stderr, "warning: invalid hex in hash\n");
                valid = 0;
                break;
            }
            entries[entry_count].hash.hash[i] = (uint8_t)byte;
        }
        
        if (!valid) continue;
        
        strncpy(entries[entry_count].path, path, sizeof(entries[entry_count].path) - 1);
        entries[entry_count].path[sizeof(entries[entry_count].path) - 1] = '\0';
        
        indices[entry_count] = entry_count;
        entry_count++;
    }
    fclose(f);

    if (entry_count == 0) {
        fprintf(stderr, "error: no entries in index\n");
        free(entries);
        free(indices);
        return -1;
    }

    // Build tree from entries
    int rc = write_tree_level_helper(entries, indices, entry_count, 0, id_out);
    free(entries);
    free(indices);
    return rc;
}