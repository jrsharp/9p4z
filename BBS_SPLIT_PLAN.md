# 9BBS Repository Split Plan

## Purpose
Split 9BBS (bulletin board system) into its own repository, keeping 9p4z as a minimal protocol library.

## Complete Inventory of BBS-Related Files

### Core BBS Implementation (MUST MOVE)
```
include/zephyr/9bbs/
  ├── 9bbs.h                      # Main BBS API
  └── chat.h                      # Chat subsystem API

src/9bbs/
  ├── 9bbs.c                      # BBS implementation (~1000 lines)
  └── chat.c                      # Chat implementation (~350 lines)
```

### BBS-Specific Documentation (MUST MOVE)
```
Root:
  ├── SPEC.md                     # 9BBS specification
  ├── IMPLEMENTATION_GUIDE.md     # Implementation guide
  └── CHAT_INTEGRATION.md         # Chat integration guide

docs/:
  ├── 9BBS_IMPLEMENTATION.md      # Implementation notes
  ├── IOS_CHAT_CLIENT_GUIDE.md    # iOS client guide
  ├── PORTABLE_BBS_ARCHITECTURE.md # Architecture doc
  └── UNIX_BBS_SERVER_SPEC.md     # Unix server spec
```

### BBS-Specific Sample (MUST MOVE)
```
samples/9bbs_demo/
  ├── CMakeLists.txt
  ├── prj.conf
  ├── README.md
  └── src/main.c                  # Demo application
```

### Shared Sample (DECISION NEEDED)
```
samples/9p_server_l2cap/        # Uses BBS conditionally
  - Contains: #ifdef CONFIG_NINEP_9BBS
  - Decision: Keep in 9p4z, optionally depend on 9bbs
```

### Configuration (EXTRACT SECTIONS)
```
Kconfig:
  - Lines 277-377: NINEP_9BBS section + 9BBS_CHAT section

CMakeLists.txt:
  - Lines 76-82: 9bbs build section
```

### Untracked Files to Preserve
```
scripts/setup-ncs-l2cap.sh      # Keep in 9p4z (L2CAP-related)
```

## New Repository Structure

### 9bbs/ (new repo)
```
9bbs/
├── CMakeLists.txt              # Main build file
├── Kconfig                     # BBS-specific config
├── README.md                   # BBS overview
├── LICENSE                     # MIT (copy from 9p4z)
│
├── zephyr/
│   └── module.yml              # Zephyr module definition
│
├── west.yml                    # Import 9p4z dependency
│
├── include/zephyr/9bbs/
│   ├── 9bbs.h
│   └── chat.h
│
├── src/
│   ├── 9bbs.c
│   └── chat.c
│
├── samples/
│   └── bbs_server_l2cap/       # Full BBS demo (moved from 9bbs_demo)
│       ├── CMakeLists.txt
│       ├── prj.conf
│       ├── README.md
│       └── src/main.c
│
└── docs/
    ├── SPEC.md
    ├── IMPLEMENTATION_GUIDE.md
    ├── CHAT_INTEGRATION.md
    ├── IOS_CHAT_CLIENT_GUIDE.md
    ├── PORTABLE_BBS_ARCHITECTURE.md
    ├── UNIX_BBS_SERVER_SPEC.md
    └── 9BBS_IMPLEMENTATION.md
```

### 9p4z/ (cleaned up)
```
9p4z/
├── CMakeLists.txt              # Remove 9bbs section (lines 76-82)
├── Kconfig                     # Remove 9bbs section (lines 277-377)
├── README.md                   # Update to remove BBS references
│
├── include/zephyr/9p/          # Keep all protocol headers
├── src/                        # Keep all protocol implementations
│   ├── proto.c
│   ├── fid.c, tag.c
│   ├── server.c, client.c
│   ├── transport_*.c           # ALL transports
│   ├── namespace.c
│   ├── sysfs.c
│   └── ... (all other non-BBS files)
│
├── samples/                    # Keep all non-BBS samples
│   ├── 9p_client/
│   ├── 9p_server/
│   ├── 9p_server_tcp/
│   ├── 9p_server_l2cap/        # KEEP - optionally uses BBS
│   ├── 9p_server_coap/
│   └── ... (other samples)
│
└── docs/                       # Keep non-BBS docs
    ├── L2CAP_TRANSPORT_DESIGN.md
    ├── IOS_IMPL_SUMMARY.md
    ├── 9PIS_GATT_SPECIFICATION.md
    └── ... (other protocol docs)
```

## Migration Steps

### Phase 1: Preparation (Current State)
- [x] Create this inventory document
- [ ] Commit all current changes in 9p4z (clean slate)
- [ ] Create backup branch: `git checkout -b pre-bbs-split`

### Phase 2: Create 9bbs Repository
- [ ] Create new directory: `/Users/jrsharp/src/9bbs/`
- [ ] Initialize git repo
- [ ] Create basic structure (CMakeLists.txt, Kconfig, zephyr/module.yml, west.yml)
- [ ] Copy BBS files from 9p4z
- [ ] Update include paths and dependencies
- [ ] Test build with west dependency on 9p4z

### Phase 3: Clean Up 9p4z
- [ ] Remove BBS files (src/9bbs/, include/zephyr/9bbs/)
- [ ] Remove BBS docs (SPEC.md, IMPLEMENTATION_GUIDE.md, etc.)
- [ ] Remove 9bbs_demo sample
- [ ] Remove BBS sections from Kconfig (lines 277-377)
- [ ] Remove BBS sections from CMakeLists.txt (lines 76-82)
- [ ] Update 9p_server_l2cap to optionally depend on 9bbs
- [ ] Update README.md
- [ ] Commit: "Split 9BBS into separate repository"

### Phase 4: Verification
- [ ] Build 9p4z samples (verify no BBS dependencies)
- [ ] Build 9bbs sample (verify 9p4z dependency works)
- [ ] Check git status for untracked files
- [ ] Compare file counts before/after

## File Move Commands (for reference)

```bash
# From 9p4z to 9bbs
mv include/zephyr/9bbs/ ../9bbs/include/zephyr/
mv src/9bbs/ ../9bbs/src/
mv samples/9bbs_demo/ ../9bbs/samples/bbs_server_l2cap/
mv SPEC.md IMPLEMENTATION_GUIDE.md CHAT_INTEGRATION.md ../9bbs/docs/
mv docs/9BBS_IMPLEMENTATION.md docs/IOS_CHAT_CLIENT_GUIDE.md \
   docs/PORTABLE_BBS_ARCHITECTURE.md docs/UNIX_BBS_SERVER_SPEC.md \
   ../9bbs/docs/
```

## West Dependency Configuration

### 9bbs/west.yml
```yaml
manifest:
  remotes:
    - name: zephyrproject-rtos
      url-base: https://github.com/zephyrproject-rtos
    - name: jrsharp
      url-base: https://github.com/jrsharp  # Or your git server

  projects:
    - name: zephyr
      remote: zephyrproject-rtos
      revision: main
      import: true

    - name: 9p4z
      remote: jrsharp
      revision: main
      path: modules/9p4z

  self:
    path: 9bbs
```

### 9bbs/zephyr/module.yml
```yaml
name: 9bbs
build:
  cmake: .
  kconfig: Kconfig
```

## Testing Checklist

### Test 9p4z (without BBS)
- [ ] `west build -p -b native_posix samples/9p_server_tcp`
- [ ] `west build -p -b native_posix samples/9p_client`
- [ ] `west build -p -b qemu_x86 samples/9p_server`
- [ ] Verify no BBS headers are accessible
- [ ] Check that CONFIG_NINEP_9BBS is not available in Kconfig

### Test 9bbs (with 9p4z dependency)
- [ ] `cd /path/to/9bbs-workspace`
- [ ] `west init -l 9bbs`
- [ ] `west update`
- [ ] `west build -p -b native_posix samples/bbs_server_l2cap`
- [ ] Verify 9p4z headers are accessible
- [ ] Verify BBS functionality works

### Test 9p_server_l2cap (optional BBS)
- [ ] Build without BBS: `CONFIG_NINEP_9BBS=n`
- [ ] Build with BBS: `CONFIG_NINEP_9BBS=y` (requires 9bbs in workspace)

## Rollback Plan

If something goes wrong:
```bash
cd /Users/jrsharp/src/9p4z
git checkout pre-bbs-split
# Or:
git reset --hard <commit-before-split>
```

## Success Criteria

- [ ] 9p4z builds without BBS code
- [ ] 9bbs builds with 9p4z as dependency
- [ ] No files lost (verify with file count)
- [ ] All documentation preserved
- [ ] Git history preserved where possible
- [ ] Both repos have clean git status

## Notes

- The `9p_server_l2cap` sample will remain in 9p4z as it's primarily an L2CAP transport demo
- It can optionally include BBS if the 9bbs module is available in the workspace
- This maintains flexibility while keeping repos focused
