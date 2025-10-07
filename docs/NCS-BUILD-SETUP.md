# Building 9p4z with Nordic NCS (nRF7002dk)

## Quick Start

```bash
# Build
/Users/jrsharp/zephyr-workspaces/build-ncs.sh

# Flash
cd /opt/nordic/ncs/v3.1.1
export PATH="/opt/nordic/ncs/toolchains/561dce9adf/bin:/opt/nordic/ncs/toolchains/561dce9adf/nrfutil/bin:$PATH"
west flash -d /Users/jrsharp/zephyr-workspaces/9p4z-ncs-workspace/build/9p_server_tcp/9p_server_tcp --runner jlink
```

## Architecture

### Key Design Decisions

1. **No west.yml modification**: We do NOT add 9p4z to `/opt/nordic/ncs/v3.1.1/nrf/west.yml`
   - Avoids corrupting NCS installation
   - Module loaded via `ZEPHYR_EXTRA_MODULES` cmake argument instead

2. **Clean CMakeLists.txt**: `samples/9p_server_tcp/CMakeLists.txt` has NO module path manipulation
   - No `list(APPEND ZEPHYR_EXTRA_MODULES ...)`
   - Build system handles module loading via cmake args

3. **Direct source reference**: Build references `/Users/jrsharp/src/9p4z/` directly
   - Changes in source repo immediately reflected in builds
   - No copy/sync required
   - No workspace confusion

## Directory Structure

```
/Users/jrsharp/src/9p4z/                    # Source repo (git)
  ├── samples/9p_server_tcp/                # Sample application
  │   ├── CMakeLists.txt                    # Clean, no module path hacks
  │   ├── prj_nrf.conf                      # nRF7002dk config
  │   └── boards/nrf7002dk_nrf5340_cpuapp.conf  # Board-specific WiFi config
  └── ...

/Users/jrsharp/zephyr-workspaces/
  ├── build-ncs.sh                          # Build script
  └── 9p4z-ncs-workspace/
      └── build/9p_server_tcp/              # Build output
          └── 9p_server_tcp/zephyr/
              ├── zephyr.hex                # Flash this (686KB)
              ├── zephyr.elf
              └── zephyr.bin

/opt/nordic/ncs/v3.1.1/                     # NCS installation
  ├── nrf/west.yml                          # CLEAN - no 9p4z entry!
  ├── zephyr/
  └── ...
```

## Build Script

Location: `/Users/jrsharp/zephyr-workspaces/build-ncs.sh`

```bash
#!/bin/bash
# Build script for 9p4z on NCS

cd /opt/nordic/ncs/v3.1.1
export PATH="/opt/nordic/ncs/toolchains/561dce9adf/bin:/opt/nordic/ncs/toolchains/561dce9adf/Cellar/python@3.12/3.12.4/Frameworks/Python.framework/Versions/3.12/bin:$PATH"
export PATH="/opt/nordic/ncs/toolchains/561dce9adf/nrfutil/bin:$PATH"

# Build 9P server TCP sample for nRF7002dk
# Use a build directory outside of NCS to avoid conflicts
/opt/nordic/ncs/toolchains/561dce9adf/Cellar/python@3.12/3.12.4/Frameworks/Python.framework/Versions/3.12/bin/west build \
  -b nrf7002dk/nrf5340/cpuapp \
  -d /Users/jrsharp/zephyr-workspaces/9p4z-ncs-workspace/build/9p_server_tcp \
  /Users/jrsharp/src/9p4z/samples/9p_server_tcp \
  --pristine \
  -- -DCONF_FILE=prj_nrf.conf -DZEPHYR_EXTRA_MODULES=/Users/jrsharp/src/9p4z
```

**Key Arguments**:
- `-DCONF_FILE=prj_nrf.conf`: Uses nRF-specific configuration
- `-DZEPHYR_EXTRA_MODULES=/Users/jrsharp/src/9p4z`: Loads 9p4z module without west.yml

## Board Configuration

### prj_nrf.conf
Location: `samples/9p_server_tcp/prj_nrf.conf`

Enables:
- 9P protocol (server, TCP transport)
- WiFi with WPA supplicant
- Network shell for WiFi management
- TLS/DTLS support

### Board-specific config
Location: `samples/9p_server_tcp/boards/nrf7002dk_nrf5340_cpuapp.conf`

```kconfig
# nRF7002dk/nRF5340 board specific configuration

# WiFi support
CONFIG_WIFI=y
CONFIG_WIFI_NRF700X=y
CONFIG_NET_L2_WIFI_MGMT=y
CONFIG_NET_L2_WIFI_SHELL=y

# Network shell
CONFIG_NET_SHELL=y

# Nordic-specific
CONFIG_HEAP_MEM_POOL_SIZE=131072
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048
```

## Flashing

### Using JLink (Recommended)
```bash
cd /opt/nordic/ncs/v3.1.1
export PATH="/opt/nordic/ncs/toolchains/561dce9adf/bin:/opt/nordic/ncs/toolchains/561dce9adf/nrfutil/bin:$PATH"
west flash -d /Users/jrsharp/zephyr-workspaces/9p4z-ncs-workspace/build/9p_server_tcp/9p_server_tcp --runner jlink
```

### Using nrfutil (Alternative)
```bash
cd /opt/nordic/ncs/v3.1.1
export PATH="/opt/nordic/ncs/toolchains/561dce9adf/nrfutil/bin:$PATH"
nrfutil device program --firmware /Users/jrsharp/zephyr-workspaces/9p4z-ncs-workspace/build/9p_server_tcp/9p_server_tcp/zephyr/zephyr.hex --core Application
```

## Usage After Boot

### Connect to WiFi
```
uart:~$ wifi scan
uart:~$ wifi connect -s "YourSSID" -p "password" -k 4
uart:~$ net iface
```

### Start 9P Server
The server starts automatically on boot and listens on port 564.

### Check Status
```
uart:~$ net tcp
uart:~$ net conn
```

## Troubleshooting

### Build fails with "undefined symbol NINEP"
- Check that `-DZEPHYR_EXTRA_MODULES=/Users/jrsharp/src/9p4z` is in the west build command
- Verify `/Users/jrsharp/src/9p4z/zephyr/module.yml` exists

### West manifest errors
- Ensure `/opt/nordic/ncs/v3.1.1/nrf/west.yml` has NO 9p4z entry
- If corrupted, remove any lines referencing 9p4z (around line 289)

### WiFi not working
- Ensure `boards/nrf7002dk_nrf5340_cpuapp.conf` exists
- Check that `CONFIG_WIFI_NRF700X=y` is set

### Code changes not taking effect
- Build system references source directly: `/Users/jrsharp/src/9p4z/`
- Just run `/Users/jrsharp/zephyr-workspaces/build-ncs.sh` again
- No need to copy or sync anything!

## Development Workflow

1. Edit code in `/Users/jrsharp/src/9p4z/`
2. Run `/Users/jrsharp/zephyr-workspaces/build-ncs.sh`
3. Flash with west
4. Test on hardware

**No intermediate steps. No sync issues. No confusion.**

## Historical Context

This setup was established after trying multiple approaches:
- ❌ ESP32-S3: WiFi support broken in mainline Zephyr (hal_espressif WPA supplicant issues)
- ❌ NCS with west.yml modification: Corrupts NCS installation, hard to maintain
- ✅ **NCS with ZEPHYR_EXTRA_MODULES**: Clean, maintainable, works perfectly

Date established: October 2025
Last verified: October 7, 2025
