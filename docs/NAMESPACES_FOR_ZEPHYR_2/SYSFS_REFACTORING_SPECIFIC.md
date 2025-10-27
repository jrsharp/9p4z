# Refactoring 9p4z sysfs for New Namespace Architecture

## Current Implementation Analysis

Your `sysfs.c` is already **very close** to what we need! It's already implementing a 9P filesystem interface. Here's what you have:

### Current Architecture
```
sysfs.c: Implements ninep_fs_ops (9P filesystem operations)
  ├─ Data: struct ninep_sysfs with entries table
  ├─ API: ninep_sysfs_register_file()
  ├─ API: ninep_sysfs_register_writable_file()
  └─ API: ninep_sysfs_register_dir()
```

### Key Insight

**Your sysfs is ALREADY a 9P filesystem!** It just needs to be wrapped as a 9P **server** rather than just implementing the ops.

## The Migration

You need to make sysfs an **in-process 9P server** that can:
1. Be registered with `ninep_server_register()`
2. Be posted to `/srv` for discovery
3. Be mounted via the namespace API

## Step-by-Step Refactoring

### Step 1: Understand What Stays the Same

**KEEP ALL OF THIS** - it's perfect:
- ✅ `struct ninep_sysfs` - your main data structure
- ✅ `struct ninep_sysfs_entry` - entry registration
- ✅ `ninep_sysfs_register_file()` - driver API
- ✅ `ninep_sysfs_register_writable_file()` - driver API
- ✅ `ninep_sysfs_register_dir()` - driver API
- ✅ All the internal logic (walk, read, write, stat)

**Drivers don't need to change at all!**

### Step 2: Create 9P Server Wrapper

Add this new code to wrap sysfs as an in-process 9P server:

```c
/* Add to sysfs.c */

/* 9P server operations for sysfs */

static int sysfs_server_version(void *priv, uint32_t msize, const char *version) {
	/* Accept 9P2000 */
	if (strcmp(version, "9P2000") == 0 || strcmp(version, "9P2000.L") == 0) {
		return 0;
	}
	return -EINVAL;
}

static int sysfs_server_attach(void *priv, uint32_t fid, uint32_t afid,
                                const char *uname, const char *aname,
                                struct ninep_qid *qid) {
	struct ninep_sysfs *sysfs = priv;
	
	/* Return root qid */
	*qid = sysfs->root->qid;
	
	/* Associate FID with root node */
	/* (You'll need FID management - see Step 3) */
	
	return 0;
}

static int sysfs_server_walk(void *priv, uint32_t fid, uint32_t newfid,
                              uint16_t nwname, char **wname,
                              struct ninep_qid *wqid) {
	struct ninep_sysfs *sysfs = priv;
	
	/* Get node for fid */
	struct ninep_fs_node *node = get_node_for_fid(sysfs, fid);
	if (!node) return -ENOENT;
	
	/* Walk the path using your existing walk function */
	struct ninep_fs_node *current = node;
	int walked = 0;
	
	for (int i = 0; i < nwname; i++) {
		current = sysfs_walk(current, wname[i], strlen(wname[i]), sysfs);
		if (!current) {
			return walked;  /* Partial walk */
		}
		
		wqid[i] = current->qid;
		walked++;
	}
	
	/* Associate newfid with walked-to node */
	associate_fid(sysfs, newfid, current);
	
	return walked;
}

static int sysfs_server_open(void *priv, uint32_t fid, uint8_t mode,
                              struct ninep_qid *qid, uint32_t *iounit) {
	struct ninep_sysfs *sysfs = priv;
	
	struct ninep_fs_node *node = get_node_for_fid(sysfs, fid);
	if (!node) return -ENOENT;
	
	/* Use your existing open function */
	int ret = sysfs_open(node, mode, sysfs);
	if (ret < 0) return ret;
	
	*qid = node->qid;
	*iounit = 8192;
	
	return 0;
}

static int sysfs_server_read(void *priv, uint32_t fid, uint64_t offset,
                              uint32_t count, void *data, uint32_t *nread) {
	struct ninep_sysfs *sysfs = priv;
	
	struct ninep_fs_node *node = get_node_for_fid(sysfs, fid);
	if (!node) return -ENOENT;
	
	/* Use your existing read function */
	int ret = sysfs_read(node, offset, data, count, sysfs);
	if (ret < 0) return ret;
	
	*nread = ret;
	return 0;
}

static int sysfs_server_write(void *priv, uint32_t fid, uint64_t offset,
                               const void *data, uint32_t count, uint32_t *nwritten) {
	struct ninep_sysfs *sysfs = priv;
	
	struct ninep_fs_node *node = get_node_for_fid(sysfs, fid);
	if (!node) return -ENOENT;
	
	/* Use your existing write function */
	int ret = sysfs_write(node, offset, data, count, sysfs);
	if (ret < 0) return ret;
	
	*nwritten = ret;
	return 0;
}

static int sysfs_server_clunk(void *priv, uint32_t fid) {
	struct ninep_sysfs *sysfs = priv;
	
	/* Free FID */
	free_fid(sysfs, fid);
	
	return 0;
}

static int sysfs_server_stat(void *priv, uint32_t fid, struct ninep_stat *stat) {
	struct ninep_sysfs *sysfs = priv;
	
	struct ninep_fs_node *node = get_node_for_fid(sysfs, fid);
	if (!node) return -ENOENT;
	
	/* Fill in stat structure */
	stat->qid = node->qid;
	stat->mode = node->mode;
	stat->length = node->length;
	strncpy(stat->name, node->name, sizeof(stat->name));
	/* ... other stat fields ... */
	
	return 0;
}

static const struct ninep_server_ops sysfs_server_ops = {
	.version = sysfs_server_version,
	.attach = sysfs_server_attach,
	.walk = sysfs_server_walk,
	.open = sysfs_server_open,
	.read = sysfs_server_read,
	.write = sysfs_server_write,
	.clunk = sysfs_server_clunk,
	.stat = sysfs_server_stat,
	/* .wstat, .remove, .create - can be NULL for read-only or add later */
};
```

### Step 3: Add FID Management

Your sysfs needs to track which FIDs map to which nodes:

```c
/* Add to struct ninep_sysfs in sysfs.h */
struct ninep_sysfs {
	/* ... existing fields ... */
	
	/* FID table */
	struct {
		struct ninep_fs_node *node;
		bool in_use;
	} fids[CONFIG_NINEP_MAX_FIDS];  // e.g., 64
	
	struct k_mutex fid_lock;
};

/* Add these helper functions to sysfs.c */

static struct ninep_fs_node *get_node_for_fid(struct ninep_sysfs *sysfs, uint32_t fid) {
	if (fid >= CONFIG_NINEP_MAX_FIDS) return NULL;
	
	k_mutex_lock(&sysfs->fid_lock, K_FOREVER);
	struct ninep_fs_node *node = sysfs->fids[fid].in_use ? sysfs->fids[fid].node : NULL;
	k_mutex_unlock(&sysfs->fid_lock);
	
	return node;
}

static int associate_fid(struct ninep_sysfs *sysfs, uint32_t fid, struct ninep_fs_node *node) {
	if (fid >= CONFIG_NINEP_MAX_FIDS) return -EINVAL;
	
	k_mutex_lock(&sysfs->fid_lock, K_FOREVER);
	sysfs->fids[fid].node = node;
	sysfs->fids[fid].in_use = true;
	k_mutex_unlock(&sysfs->fid_lock);
	
	return 0;
}

static void free_fid(struct ninep_sysfs *sysfs, uint32_t fid) {
	if (fid >= CONFIG_NINEP_MAX_FIDS) return;
	
	k_mutex_lock(&sysfs->fid_lock, K_FOREVER);
	sysfs->fids[fid].in_use = false;
	sysfs->fids[fid].node = NULL;
	k_mutex_unlock(&sysfs->fid_lock);
}
```

### Step 4: Add Server Registration API

Add a new function to register sysfs as a 9P server:

```c
/* Add to sysfs.h */

/**
 * Register sysfs as a 9P server and mount in namespace
 * 
 * @param sysfs Initialized sysfs instance
 * @param mount_point Where to mount (e.g., "/sys")
 * @return 0 on success, negative error code on failure
 */
int ninep_sysfs_register_server(struct ninep_sysfs *sysfs, const char *mount_point);
```

```c
/* Add to sysfs.c */

int ninep_sysfs_register_server(struct ninep_sysfs *sysfs, const char *mount_point) {
	if (!sysfs || !mount_point) {
		return -EINVAL;
	}
	
	/* Initialize FID management */
	k_mutex_init(&sysfs->fid_lock);
	memset(sysfs->fids, 0, sizeof(sysfs->fids));
	
	/* Register as 9P server */
	struct ninep_server *server = ninep_server_register("sys", &sysfs_server_ops, sysfs);
	if (!server) {
		LOG_ERR("Failed to register sysfs as 9P server");
		return -ENOMEM;
	}
	
	/* Post to /srv */
	int ret = srv_post("sys", server);
	if (ret < 0) {
		LOG_ERR("Failed to post sysfs to /srv: %d", ret);
		return ret;
	}
	
	/* Mount in default namespace */
	ret = srv_mount("sys", mount_point, 0);
	if (ret < 0) {
		LOG_ERR("Failed to mount sysfs at %s: %d", mount_point, ret);
		return ret;
	}
	
	LOG_INF("Sysfs registered and mounted at %s", mount_point);
	return 0;
}
```

### Step 5: Update Initialization

Change how sysfs is initialized in your system:

**Before:**
```c
/* Current usage (somewhere in your system init) */
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry entries[32];

void system_init(void) {
	ninep_sysfs_init(&sysfs, entries, 32);
	
	/* Drivers register files */
	ninep_sysfs_register_file(&sysfs, "version", version_gen, NULL);
	ninep_sysfs_register_file(&sysfs, "uptime", uptime_gen, NULL);
	
	/* ... somehow integrate with VFS or 9P ... */
}
```

**After:**
```c
/* New usage */
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry entries[32];

void system_init(void) {
	/* Initialize sysfs */
	ninep_sysfs_init(&sysfs, entries, 32);
	
	/* Drivers register files - SAME AS BEFORE! */
	ninep_sysfs_register_file(&sysfs, "version", version_gen, NULL);
	ninep_sysfs_register_file(&sysfs, "uptime", uptime_gen, NULL);
	
	/* NEW: Register as 9P server and mount */
	ninep_sysfs_register_server(&sysfs, "/sys");
	
	/* Done! Now accessible via namespace API */
}
```

## Benefits

### Before Migration:
- ❌ Custom integration (filesystem ops directly called)
- ❌ Not network-transparent
- ❌ Not discoverable
- ❌ Global, not per-namespace

### After Migration:
- ✅ Standard 9P server (works with namespace system)
- ✅ Network-transparent (can be accessed remotely!)
- ✅ Discoverable via `/srv/sys`
- ✅ Mountable in per-thread namespaces
- ✅ **Same driver API** - no driver changes needed!

## Usage Examples

### Local Access (Same as Before)
```c
/* Application code - works via namespace API now */
int fd = ns_open("/sys/version", FS_O_READ);
char buf[64];
ns_read(fd, buf, sizeof(buf));
ns_close(fd);
```

### Service Discovery (NEW!)
```c
/* List available services */
int dir = ns_opendir("/srv");
struct fs_dirent entry;
while (ns_readdir(dir, &entry) == 0) {
	printk("Service: %s\n", entry.name);
}
// Output: "sys", "display", etc.
```

### Network Access (NEW!)
```c
/* On Device B: Mount Device A's sysfs */
srv_post_network("deviceA_sys", "tcp", "192.168.1.100:564");
srv_mount("deviceA_sys", "/remote/sys", 0);

/* Read remote sysfs */
int fd = ns_open("/remote/sys/version", FS_O_READ);
// Reading Device A's version remotely!
```

### Per-Thread Namespaces (NEW!)
```c
/* Debug thread has custom sysfs view */
void debug_thread(void) {
	ns_create(NULL);  // Create custom namespace
	
	/* Mount extended debug sysfs */
	srv_mount("sys_debug", "/sys", 0);
	
	/* This thread sees debug files that others don't */
	int fd = ns_open("/sys/debug/trace", FS_O_READ);
}
```

## File Changes Summary

### Modified Files:
1. **include/zephyr/9p/sysfs.h**
   - Add FID table to `struct ninep_sysfs`
   - Add `ninep_sysfs_register_server()` function

2. **src/sysfs.c**
   - Add server ops wrapper (`sysfs_server_*` functions)
   - Add FID management functions
   - Add `ninep_sysfs_register_server()` implementation

### No Changes Needed:
- ✅ Driver code (keep using `ninep_sysfs_register_file()`)
- ✅ Core sysfs logic (walk, read, write, stat)
- ✅ Entry registration
- ✅ Generator/writer callbacks

## Migration Checklist

- [ ] Add `struct ninep_server_ops` wrapper functions
- [ ] Add FID management to `struct ninep_sysfs`
- [ ] Implement FID helper functions
- [ ] Add `ninep_sysfs_register_server()` function
- [ ] Update system init to call register_server()
- [ ] Test local access works
- [ ] Test `/srv` discovery works
- [ ] Test namespace mounting works
- [ ] Test network access works
- [ ] Update documentation

## Testing Strategy

### Phase 1: Backward Compatibility
```c
/* Ensure existing functionality still works */
void test_sysfs_compat(void) {
	/* Register files */
	ninep_sysfs_register_file(&sysfs, "test", test_gen, NULL);
	
	/* Access via namespace API */
	int fd = ns_open("/sys/test", FS_O_READ);
	assert(fd >= 0);
	
	char buf[32];
	ssize_t n = ns_read(fd, buf, sizeof(buf));
	assert(n > 0);
	
	ns_close(fd);
}
```

### Phase 2: Server Features
```c
/* Test 9P server functionality */
void test_sysfs_server(void) {
	/* Check /srv registration */
	int dir = ns_opendir("/srv");
	bool found_sys = false;
	struct fs_dirent entry;
	
	while (ns_readdir(dir, &entry) == 0) {
		if (strcmp(entry.name, "sys") == 0) {
			found_sys = true;
			break;
		}
	}
	assert(found_sys);
	ns_closedir(dir);
}
```

### Phase 3: Network Access
```c
/* Test remote access */
void test_sysfs_remote(void) {
	/* Start 9P server on network */
	ninep_server_export(sysfs_server, "tcp", "0.0.0.0:564");
	
	/* On another device/thread: */
	srv_post_network("remote_sys", "tcp", "localhost:564");
	srv_mount("remote_sys", "/remote/sys", 0);
	
	int fd = ns_open("/remote/sys/version", FS_O_READ);
	assert(fd >= 0);
	// Success - reading remote sysfs!
}
```

## Estimated Effort

- **Code changes:** ~200 lines of new code (mostly wrappers)
- **Testing:** 1-2 days
- **Total time:** 2-3 days for complete migration
- **Risk:** Very low (existing code unchanged, just wrapped)

## Key Insight

Your sysfs implementation is **already 90% there!** You just need to:
1. Wrap it as a 9P server (add server ops)
2. Add FID management
3. Register with `/srv` and namespace

The hard work (walk, read, write, stat, entry management) is already done!

## Summary

**What Changes:**
- Add 9P server wrapper layer
- Add FID-to-node mapping
- Register as server and post to `/srv`

**What Stays Same:**
- All driver APIs
- All internal logic
- Entry management
- Generator/writer callbacks

**What You Get:**
- Network transparency
- Service discovery
- Per-thread namespaces
- Integration with new architecture

This is a **high-reward, low-risk refactoring** that makes your existing sysfs work with the full Plan 9 namespace system!
