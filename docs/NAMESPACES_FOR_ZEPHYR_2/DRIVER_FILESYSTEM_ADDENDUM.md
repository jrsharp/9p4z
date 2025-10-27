# Addendum: Driver-as-Filesystem Model

## Overview

This addendum extends the main specification to support the Plan 9 paradigm where **drivers expose themselves as filesystems**. This allows device drivers to be accessed through the standard file I/O interface and mounted anywhere in the namespace.

## Motivation

In Plan 9, everything truly is a file:
- `/dev/draw` - Display driver (graphics operations via file I/O)
- `/dev/mouse` - Mouse events (read mouse events as file data)
- `/net/tcp` - TCP stack (open a connection = open a file)
- `/proc/123/ctl` - Process control (write commands to control process)

This same model should work in Zephyr, allowing drivers to:
1. Register as 9P filesystems
2. Be mounted in any thread's namespace
3. Be accessed via standard `ns_open()`, `ns_read()`, `ns_write()` calls
4. Be composed with union mounts
5. Be accessed remotely (another device can mount your driver over 9P!)

## Architecture

### In-Process 9P Servers

Drivers implement a **lightweight 9P server** that runs in the same process:

```
┌──────────────────────────────────────┐
│  Application Thread                   │
│  ns_open("/dev/draw")                 │
└──────────────────────────────────────┘
               ↓
┌──────────────────────────────────────┐
│  Namespace Layer                      │
│  Resolves to in-process 9P mount     │
└──────────────────────────────────────┘
               ↓
┌──────────────────────────────────────┐
│  9P VFS Driver                        │
│  Calls into 9P "transport"           │
└──────────────────────────────────────┘
               ↓
┌──────────────────────────────────────┐
│  In-Process 9P Server                │
│  (Display Driver's 9P interface)     │
└──────────────────────────────────────┘
               ↓
┌──────────────────────────────────────┐
│  Display Driver Implementation        │
│  (actual hardware control)           │
└──────────────────────────────────────┘
```

### Transport: "local" (In-Process)

The 9p4z library needs a new transport type for in-process 9P servers:

```c
/**
 * Local (in-process) transport
 * Directly calls into a 9P server running in the same address space
 */
struct ninep_transport_local {
    struct ninep_server *server;  /* Pointer to in-process 9P server */
};
```

## Data Structures

### 9P Server Interface

```c
/**
 * 9P server operations
 * Drivers implement this interface to expose themselves as filesystems
 */
struct ninep_server_ops {
    /* Called when client sends Tversion */
    int (*version)(void *priv, uint32_t msize, const char *version);
    
    /* Called when client sends Tattach */
    int (*attach)(void *priv, uint32_t fid, uint32_t afid, 
                  const char *uname, const char *aname,
                  struct ninep_qid *qid);
    
    /* Called when client sends Twalk */
    int (*walk)(void *priv, uint32_t fid, uint32_t newfid,
                uint16_t nwname, char **wname,
                struct ninep_qid *wqid);
    
    /* Called when client sends Topen */
    int (*open)(void *priv, uint32_t fid, uint8_t mode,
                struct ninep_qid *qid, uint32_t *iounit);
    
    /* Called when client sends Tread */
    int (*read)(void *priv, uint32_t fid, uint64_t offset,
                uint32_t count, void *data, uint32_t *nread);
    
    /* Called when client sends Twrite */
    int (*write)(void *priv, uint32_t fid, uint64_t offset,
                 const void *data, uint32_t count, uint32_t *nwritten);
    
    /* Called when client sends Tclunk */
    int (*clunk)(void *priv, uint32_t fid);
    
    /* Called when client sends Tstat */
    int (*stat)(void *priv, uint32_t fid, struct ninep_stat *stat);
    
    /* Called when client sends Twstat */
    int (*wstat)(void *priv, uint32_t fid, const struct ninep_stat *stat);
    
    /* Called when client sends Tremove */
    int (*remove)(void *priv, uint32_t fid);
    
    /* Called when client sends Tcreate */
    int (*create)(void *priv, uint32_t fid, const char *name,
                  uint32_t perm, uint8_t mode,
                  struct ninep_qid *qid, uint32_t *iounit);
};

/**
 * 9P server instance
 */
struct ninep_server {
    const struct ninep_server_ops *ops;  /* Server operations */
    void *priv;                           /* Driver private data */
    struct k_mutex lock;                  /* Serialization */
    
    /* FID table for tracking open files */
    struct {
        void *file_ctx;    /* Driver's per-file context */
        bool in_use;
    } fids[CONFIG_NINEP_SERVER_MAX_FIDS];
};
```

### Driver Registration

```c
/**
 * Register a driver as a 9P server that can be mounted
 * 
 * @param name   Unique name for this server (e.g., "draw", "mouse")
 * @param ops    9P server operations implemented by driver
 * @param priv   Driver private data
 * @return       Server instance, or NULL on error
 */
struct ninep_server *ninep_server_register(const char *name,
                                           const struct ninep_server_ops *ops,
                                           void *priv);

/**
 * Unregister a 9P server
 */
int ninep_server_unregister(struct ninep_server *server);

/**
 * Mount an in-process 9P server into the namespace
 * 
 * @param server     Server instance to mount
 * @param mnt_point  Where to mount in namespace
 * @param flags      Namespace flags
 */
int ns_mount_server(struct ninep_server *server, const char *mnt_point,
                    uint32_t flags);
```

## Example: Display Driver

Here's how a display driver would expose itself as `/dev/draw`:

```c
/* Display driver's private data */
struct draw_driver {
    const struct device *display_dev;
    struct k_mutex lock;
    /* ... display state ... */
};

/* File contexts (one per open file) */
struct draw_file {
    enum {
        DRAW_CTL,      /* /dev/draw/ctl - control file */
        DRAW_DATA,     /* /dev/draw/data - pixel data */
        DRAW_NEW,      /* /dev/draw/new - create new context */
    } type;
    
    int context_id;    /* Which drawing context */
    /* ... per-file state ... */
};

/* 9P operations for display driver */
static int draw_attach(void *priv, uint32_t fid, uint32_t afid,
                       const char *uname, const char *aname,
                       struct ninep_qid *qid) {
    /* Return root directory of driver */
    qid->type = QTDIR;
    qid->version = 0;
    qid->path = 0;  /* Root */
    return 0;
}

static int draw_walk(void *priv, uint32_t fid, uint32_t newfid,
                     uint16_t nwname, char **wname,
                     struct ninep_qid *wqid) {
    struct draw_driver *drv = priv;
    
    /* Walking from root */
    if (nwname == 1) {
        if (strcmp(wname[0], "ctl") == 0) {
            wqid[0].type = QTFILE;
            wqid[0].path = 1;  /* ctl file */
            return 1;
        } else if (strcmp(wname[0], "data") == 0) {
            wqid[0].type = QTFILE;
            wqid[0].path = 2;  /* data file */
            return 1;
        } else if (strcmp(wname[0], "new") == 0) {
            wqid[0].type = QTFILE;
            wqid[0].path = 3;  /* new file */
            return 1;
        }
    }
    
    return -ENOENT;
}

static int draw_open(void *priv, uint32_t fid, uint8_t mode,
                     struct ninep_qid *qid, uint32_t *iounit) {
    struct draw_driver *drv = priv;
    struct draw_file *file;
    
    /* Allocate file context based on which file was opened */
    file = k_malloc(sizeof(*file));
    if (!file) return -ENOMEM;
    
    /* Store in FID table */
    drv->server->fids[fid].file_ctx = file;
    drv->server->fids[fid].in_use = true;
    
    /* ... initialize file based on qid->path ... */
    
    *iounit = 8192;
    return 0;
}

static int draw_read(void *priv, uint32_t fid, uint64_t offset,
                     uint32_t count, void *data, uint32_t *nread) {
    struct draw_driver *drv = priv;
    struct draw_file *file = drv->server->fids[fid].file_ctx;
    
    switch (file->type) {
    case DRAW_CTL:
        /* Read control info (e.g., screen dimensions) */
        *nread = snprintf(data, count, "1920x1080x32\n");
        return 0;
        
    case DRAW_DATA:
        /* Read pixel data from framebuffer */
        /* ... */
        return 0;
        
    case DRAW_NEW:
        /* Return new context ID */
        *nread = snprintf(data, count, "%d\n", file->context_id);
        return 0;
    }
    
    return -EINVAL;
}

static int draw_write(void *priv, uint32_t fid, uint64_t offset,
                      const void *data, uint32_t count, uint32_t *nwritten) {
    struct draw_driver *drv = priv;
    struct draw_file *file = drv->server->fids[fid].file_ctx;
    
    switch (file->type) {
    case DRAW_CTL:
        /* Parse control commands: "resize 800 600", "mode rgb", etc. */
        /* ... parse and execute commands ... */
        *nwritten = count;
        return 0;
        
    case DRAW_DATA:
        /* Write pixel data to framebuffer */
        /* ... */
        *nwritten = count;
        return 0;
        
    case DRAW_NEW:
        /* Can't write to 'new' file */
        return -EPERM;
    }
    
    return -EINVAL;
}

static int draw_clunk(void *priv, uint32_t fid) {
    struct draw_driver *drv = priv;
    struct draw_file *file = drv->server->fids[fid].file_ctx;
    
    /* Clean up file context */
    k_free(file);
    drv->server->fids[fid].in_use = false;
    
    return 0;
}

static const struct ninep_server_ops draw_ops = {
    .attach = draw_attach,
    .walk = draw_walk,
    .open = draw_open,
    .read = draw_read,
    .write = draw_write,
    .clunk = draw_clunk,
    /* ... other ops ... */
};

/* Driver initialization */
int draw_driver_init(const struct device *display_dev) {
    struct draw_driver *drv = k_malloc(sizeof(*drv));
    drv->display_dev = display_dev;
    k_mutex_init(&drv->lock);
    
    /* Register as 9P server */
    struct ninep_server *server = ninep_server_register("draw", &draw_ops, drv);
    drv->server = server;
    
    /* Mount in default namespace */
    ns_mount_server(server, "/dev/draw", 0);
    
    return 0;
}
```

### Usage (Application Side)

```c
void draw_rect(int x, int y, int w, int h, uint32_t color) {
    /* Open control file */
    int ctl = ns_open("/dev/draw/ctl", FS_O_RDWR);
    
    /* Send drawing command */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rect %d %d %d %d %06x", x, y, w, h, color);
    ns_write(ctl, cmd, strlen(cmd));
    
    ns_close(ctl);
}

void get_screen_info(int *width, int *height, int *depth) {
    int ctl = ns_open("/dev/draw/ctl", FS_O_READ);
    
    char buf[64];
    ssize_t n = ns_read(ctl, buf, sizeof(buf));
    
    sscanf(buf, "%dx%dx%d", width, height, depth);
    
    ns_close(ctl);
}
```

## More Driver Examples

### Mouse Driver: `/dev/mouse`

```c
/* Reading mouse events */
int mouse = ns_open("/dev/mouse", FS_O_READ);
while (1) {
    struct mouse_event {
        int32_t x, y;      /* Position */
        uint8_t buttons;   /* Button state */
    } event;
    
    ns_read(mouse, &event, sizeof(event));
    handle_mouse_event(&event);
}
```

### GPIO Driver: `/dev/gpio`

```
/dev/gpio/
    pin0/
        direction    (read/write: "in" or "out")
        value        (read/write: "0" or "1")
        edge         (write: "rising", "falling", "both")
    pin1/
        ...
```

```c
/* Configure GPIO */
int dir = ns_open("/dev/gpio/pin0/direction", FS_O_WRITE);
ns_write(dir, "out", 3);
ns_close(dir);

/* Set GPIO value */
int val = ns_open("/dev/gpio/pin0/value", FS_O_WRITE);
ns_write(val, "1", 1);  /* Set high */
ns_close(val);
```

### Network Stack: `/net`

```
/net/
    tcp/
        clone       (open to get new connection)
        0/
            ctl     (control connection)
            data    (read/write data)
            local   (read local address)
            remote  (read remote address)
            status  (read connection status)
        1/
            ...
```

```c
/* Open TCP connection */
int clone = ns_open("/net/tcp/clone", FS_O_RDWR);
char conn[16];
ns_read(clone, conn, sizeof(conn));  /* Get connection number, e.g., "0" */
ns_close(clone);

/* Connect to remote host */
char ctl_path[64];
snprintf(ctl_path, sizeof(ctl_path), "/net/tcp/%s/ctl", conn);
int ctl = ns_open(ctl_path, FS_O_WRITE);
ns_write(ctl, "connect 192.168.1.100!80", 24);
ns_close(ctl);

/* Send/receive data */
char data_path[64];
snprintf(data_path, sizeof(data_path), "/net/tcp/%s/data", conn);
int data = ns_open(data_path, FS_O_RDWR);
ns_write(data, "GET / HTTP/1.0\r\n\r\n", 18);

char response[1024];
ns_read(data, response, sizeof(response));
ns_close(data);
```

## Benefits

### 1. **Uniform Interface**
All devices accessed the same way - through file I/O. No device-specific APIs.

### 2. **Namespace Composition**
Devices can be bound and unioned like any filesystem:

```c
/* Union local and remote GPIO */
ns_bind("/remote/dev/gpio", "/dev/gpio", NS_FLAG_AFTER);

/* Now /dev/gpio shows both local and remote GPIO pins! */
```

### 3. **Network Transparency**
Drivers automatically become network-accessible:

```c
/* On Device A: Display driver registers as 9P server */
struct ninep_server *srv = ninep_server_register("draw", &draw_ops, drv);

/* Also export over TCP for remote access */
ninep_server_export(srv, "tcp", "0.0.0.0:5640");

/* On Device B: Mount remote display */
struct ninep_client *client = ninep_client_create();
ninep_connect(client, "tcp", "deviceA:5640");
struct fs_mount_t mount = {
    .type = FS_TYPE_9P,
    .fs_data = &(struct ninep_mount_ctx){ .client = *client },
};
ns_mount(&mount, "/dev/draw", 0);

/* Now Device B can draw on Device A's display! */
draw_rect(10, 10, 100, 100, 0xFF0000);
```

### 4. **Testing and Debugging**
Mock drivers for testing:

```c
/* Mock display driver for unit tests */
struct ninep_server *mock = ninep_server_register("draw_mock", &mock_ops, NULL);
ns_mount_server(mock, "/dev/draw", NS_FLAG_REPLACE);

/* Tests now run against mock without hardware */
```

### 5. **Dynamic Device Management**
Devices can appear and disappear:

```c
/* USB device plugged in */
void usb_device_added(struct usb_device *dev) {
    struct ninep_server *srv = create_usb_device_server(dev);
    ns_mount_server(srv, "/dev/usb0", 0);
}

/* USB device removed */
void usb_device_removed(struct usb_device *dev) {
    ns_unmount("/dev/usb0", NULL);
}
```

## Implementation Guidelines

### Lightweight 9P Server

The in-process 9P server should be **much simpler** than a full network 9P server:

- No network protocol handling (no T/R message framing)
- No concurrency issues (single-threaded in same address space)
- Direct function calls instead of message passing
- Minimal FID management (small, fixed pool)

### Memory Considerations

```c
/* Typical memory usage per in-process 9P server */
sizeof(struct ninep_server)  ~100 bytes
+ (MAX_FIDS * sizeof(fid_entry))  ~32 bytes each
+ driver private data  (varies)

/* Example: Display driver with 16 max open files */
Total: ~100 + (16 * 32) + driver_data = ~600 bytes + driver_data
```

### Performance

In-process 9P servers should have **minimal overhead**:

- `ns_open()` → function call to driver's `open()` handler
- `ns_read()` → function call to driver's `read()` handler
- No serialization, no network, no context switches
- Overhead: ~10-20 instructions per operation

## Integration with Existing Drivers

### Option 1: Native 9P Drivers
New drivers implement `ninep_server_ops` directly.

### Option 2: Wrapper for Zephyr Drivers
Existing Zephyr drivers can be wrapped:

```c
/* Wrapper that adapts Zephyr driver to 9P interface */
struct ninep_server *ninep_wrap_zephyr_driver(
    const struct device *dev,
    const struct device_api *api
);

/* Example: Wrap existing display driver */
const struct device *display = device_get_binding("DISPLAY");
struct ninep_server *srv = ninep_wrap_zephyr_driver(display, display_api);
ns_mount_server(srv, "/dev/display", 0);
```

## Configuration

```kconfig
config NINEP_SERVER
    bool "In-process 9P server support"
    depends on NAMESPACE
    help
      Enable support for drivers to register as in-process 9P servers.
      This allows drivers to expose themselves as filesystems that can
      be mounted in namespaces.

config NINEP_SERVER_MAX_SERVERS
    int "Maximum number of in-process 9P servers"
    depends on NINEP_SERVER
    default 16
    help
      Maximum number of drivers that can register as 9P servers.

config NINEP_SERVER_MAX_FIDS
    int "Maximum FIDs per in-process server"
    depends on NINEP_SERVER
    default 32
    help
      Maximum number of simultaneously open files per in-process 9P server.
```

## Summary

This addendum adds the critical "driver-as-filesystem" capability that makes Plan 9 so elegant:

✅ **Drivers register as 9P servers**
✅ **Mounted via standard namespace API**  
✅ **Accessed via standard file I/O**
✅ **Composable with union mounts**
✅ **Network-transparent automatically**
✅ **Lightweight in-process implementation**

This transforms Zephyr drivers from device-specific APIs into uniform, composable, network-transparent resources accessible through the filesystem interface. It's the missing piece that makes this truly Plan 9-like!
