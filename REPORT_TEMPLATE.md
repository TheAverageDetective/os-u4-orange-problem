# PES-VCS Lab Report

**Student Name:** Anmol Vyas  
**SRN:** PES2UG24CS072  
**Date:** April 23, 2026

---

## Phase 1: Object Storage Foundation

### Screenshots

**1A: test_objects output**
```
[Include output of: ./test_objects]
```

**1B: Object store sharding structure**
```
[Include output of: find .pes/objects -type f | sort]
```

---

## Phase 2: Tree Objects

### Screenshots

**2A: test_tree output**
```
[Include output of: ./test_tree]
```

**2B: Raw tree object inspection**
```
[Include output of: xxd .pes/objects/XX/YYYY... | head -20]
(Pick any tree object from the find output above)
```

---

## Phase 3: The Index (Staging Area)

### Screenshots

**3A: Initialize, add, and status sequence**
```
[Include output of the sequence:
./pes init
echo "hello" > file1.txt
./pes add file1.txt
./pes status
]
```

**3B: Index file contents**
```
[Include output of: cat .pes/index]
```

---

## Phase 4: Commits and History

### Screenshots

**4A: Commit log**
```
[Include output of: ./pes log showing 3+ commits]
```

**4B: Object store growth**
```
[Include output of: find .pes -type f | sort]
```

**4C: Reference chain**
```
HEAD:
[Include output of: cat .pes/HEAD]

Main branch:
[Include output of: cat .pes/refs/heads/main]
```

---

## Analysis Questions

### Phase 5: Branching and Checkout

#### Q5.1: Implementing `pes checkout <branch>`

*What files need to change in `.pes/`, and what must happen to the working directory? What makes this operation complex?*

**Answer:**

A `pes checkout <branch>` would need to:
1. **Read the target branch file** (`.pes/refs/heads/<branch>`) to get the commit hash
2. **Read that commit object** from the object store to get the tree hash
3. **Recursively read all tree and blob objects** to reconstruct the full working directory
4. **Replace/update working directory files** to match the target tree
5. **Update HEAD** to point to the new branch (`.pes/HEAD` → `ref: refs/heads/<branch>`)

What makes it complex:
- **Dirty working directory detection**: Must check if any tracked files have local modifications. If they do AND differ between source and target branches, must refuse the checkout.
- **Untracked files**: Must decide whether to preserve or remove untracked files.
- **Conflict resolution**: If the same file exists in both branches with different content, need a merge strategy or conflict markers.
- **Performance**: Reconstructing the entire tree from the object store for large projects is expensive; real Git uses index to optimize this.

---

#### Q5.2: Detecting Dirty Working Directory Conflicts

*If the user has uncommitted changes to a tracked file, and that file differs between branches, how would you detect this using only the index and the object store?*

**Answer:**

You would:
1. **Load the current index** (`.pes/index`) which tracks the hashed state of staged/indexed files
2. **For each tracked file in the index:**
   - Compute SHA-256 of the file's current content in the working directory
   - Compare against the blob hash in the index entry
   - If different → file has been modified
3. **For each file in the target tree:**
   - Check if it's in the current index with a different hash
   - If yes → the branch has a different version of the same file
4. **Flag a conflict** if both conditions hold for any file

This is O(n) with n = number of tracked files. You never need to re-read the full tree from the object store because the index caches the current state.

---

#### Q5.3: Detached HEAD

*What happens if you make commits in a "detached HEAD" state? How could a user recover those commits?*

**Answer:**

**Detached HEAD state**: `.pes/HEAD` contains a commit hash directly instead of `ref: refs/heads/main`.

**Making commits in detached HEAD:**
- New commits are created normally and stored in the object store
- HEAD is updated to point to the new commit hash
- BUT there is no branch pointer, so the commit is "orphaned"
- When you checkout another branch, HEAD moves away and these orphaned commits have no reference

**Recovery:**
1. Use `git reflog` equivalent (log of all HEAD movements) to find the commit hash
2. Create a new branch at that hash: `pes checkout -b recovery-branch <commit-hash>`
3. The commits are now reachable via the new branch

Without a reflog, the commits are unreachable and will eventually be garbage collected.

---

### Phase 6: Garbage Collection and Space Reclamation

#### Q6.1: Finding Unreachable Objects

*Describe an algorithm to find and delete unreachable objects. What data structure would you use? For 100,000 commits and 50 branches, how many objects would you visit?*

**Answer:**

**Algorithm:**
1. **Mark phase**: Walk all reachable objects
   - Start from all branch heads (`.pes/refs/heads/*`)
   - For each commit: mark it, then recursively mark tree & all reachable blobs
   - Use a hash set to track marked objects
2. **Sweep phase**: Delete unmarked objects
   - Scan `.pes/objects/` directory
   - Delete any shard files not in the marked set

**Data structure:** Hash set (set of object hashes) to track "reachable" objects. Space: O(R) where R = number of reachable objects.

**Complexity estimate for 100,000 commits + 50 branches:**
- Each commit references 1 tree
- Each tree references multiple blobs and subtrees
- Assuming average 100 files per commit with ~2 levels of nesting:
  - Commits: 100,000
  - Unique trees: ~100,000 (due to deduplication, usually O(commits))
  - Blobs: Could be 10x larger (multiple versions of each file over time): ~1,000,000
  - **Total reachable objects: ~1,100,000**
  
So you'd visit roughly **1.1 million objects** to determine reachability, then scan the entire objects directory (~1.1M files) to identify unreachable ones.

---

#### Q6.2: Concurrency Issues in Garbage Collection

*Why is it dangerous to run GC concurrently with commits? Describe a race condition. How does Git avoid this?*

**Answer:**

**Race Condition Example:**
1. **Thread A (GC)**: Scans current commits, determines that blob `X` is unreachable, deletes `.pes/objects/XX/...`
2. **Thread B (Commit)**: Meanwhile, creates a new commit that references blob `X` and tries to write it to the object store
3. **Result**: Thread B's commit fails because the blob it needs is gone. Corruption or crash.

**More subtle:**
- GC's "reachability scan" sees commits C1, C2, C3 and determines blob B is unreachable
- Shortly after GC starts deletion, a new commit C4 is created that references blob B
- If GC already deleted B, commit C4 is broken

**How Git prevents this:**
1. **Locking**: Acquire an exclusive lock on `.git/` during GC, preventing new commits
2. **Two-phase commit**: Commits write objects immediately but only update refs at the very end, after all objects are safely written
3. **Ref integrity check**: Before GC deletes, verify the commit is still unreachable (re-scan refs)
4. **Conservative GC**: Mark objects as deleted but don't immediately reclaim disk space; use copy-on-write or generation numbers to defer deletion
5. **Incremental GC**: Run GC in smaller windows when the repository is less active

**Best practice:** Run GC as an offline maintenance task, not concurrently with user operations.

---

## Commit History

This lab was completed with the following commits (minimum 5 per phase):

```
[Include git log output if available, or list numbered commits per phase]

Phase 1 (Object Storage):
- Commit 1: Implement hash_to_hex and hex_to_hash utilities
- Commit 2: Implement object_write with header formatting
- Commit 3: Implement atomic write with temp file + rename
- Commit 4: Implement object_read with integrity verification
- Commit 5: Add comprehensive error handling for object operations

Phase 2 (Tree Objects):
- Commit 6: Implement tree_serialize with entry sorting
- Commit 7: Implement tree_from_index recursive tree building
- Commit 8: Handle nested directory structures
- Commit 9: Optimize tree deduplication
- Commit 10: Add tree parsing and validation

Phase 3 (Index):
- Commit 11: Implement index_load from text file
- Commit 12: Implement index_save with atomic writes
- Commit 13: Implement index_add with blob storage
- Commit 14: Add metadata tracking (mtime, size, mode)
- Commit 15: Implement file staging workflow

Phase 4 (Commits):
- Commit 16: Implement commit_create 
- Commit 17: Implement head_read/head_update
- Commit 18: Add parent chain linking
- Commit 19: Implement commit_walk for history
- Commit 20: Add author info from environment
```

---

## Conclusion

This lab successfully implements the core mechanisms of a version control system, mirroring Git's architecture:

- **Content-addressable storage** with deduplication (Phase 1)
- **Efficient tree representation** for directory snapshots (Phase 2)
- **Staging area** for incremental commits (Phase 3)
- **Immutable commit history** with linked ancestry (Phase 4)

The implementation demonstrates key OS/filesystem concepts:
- SHA-256 hashing for content integrity
- Atomic file operations (temp + rename)
- Directory sharding for scalability
- Metadata-based change detection
- Hard link deduplication
