# Addendum: /srv Service Registry

## Overview

In Plan 9, `/srv` is the **service registry** - a special directory where 9P servers post named file descriptors that clients can open to establish connections. This is a critical piece of Plan 9's architecture that enables:

1. **Service discovery** - Find what services are available
2. **Service rendezvous** - Connect clients to servers
3. **Dynamic binding** - Services can come and go
4. **Network transparency** - Local and remote services treated uniformly

This addendum explains how to implement `/srv` in Zephyr.

## The Problem `/srv` Solves

Without `/srv`, you need to know connection details in advance:

```c
// How do you know where the service is?
struct ninep_client *client = ninep_client_create();
ninep_connect(client, "tcp", "192.168.1.100:564");  // Hardcoded!
```

With `/srv`, services advertise themselves and clients discover them:

```c
// Server posts itself
srv_post("sensors", server);  // Now visible as /srv/sensors

// Client discovers and connects
int fd = ns_open("/srv/sensors", FS_O_RDWR);
// Opening /srv/sensors returns a connection to the sensors service
// Now mount it
ns_mount_fd(fd, "/remote/sensors", 0);
```

## Architecture

### `/srv` as a Synthetic Filesystem

`/srv` is implemented as an **in-process synthetic filesystem** that manages service registrations:

```
/srv/
    sensors              (file representing connection to sensors service)
    display              (file representing connection to display service)
    config               (file representing connection to config service)
    tcp!192.168.1.100    (file representing network connection)
    ...
```

Each "file" in `/srv` is actually a **connection factory** - opening it creates a new connection to the service.

## Data Structures

### Service Entry

```c
/**
 * Service entry in /srv
 * Represents a named service that clients can connect to
 */
struct srv_entry {
    char name[CONFIG_SRV_MAX_NAME_LEN];    /* Service name */
    
    /* How to connect to this service */
    enum srv_type {
        SRV_TYPE_LOCAL,      /* In-process 9P server */
        SRV_TYPE_NETWORK,    /* Network connection (TCP/BLE/etc.) */
        SRV_TYPE_IPC,        /* Inter-thread message queue */
    } type;
    
    /* Connection info */
    union {
        struct {
            struct ninep_server *server;   /* For in-process servers */
        } local;
        
        struct {
            char transport[32];            /* "tcp", "ble", "uart" */
            char address[128];             /* Address string */
        } network;
        
        struct {
            k_tid_t server_tid;            /* Server thread ID */
            struct k_msgq *msgq;           /* Message queue */
        } ipc;
    };
    
    /* Metadata */
    uid_t owner;                           /* Who posted this service */
    uint32_t flags;
    
    /* Reference counting */
    atomic_t refcount;
    
    /* List linkage */
    struct srv_entry *next;
};

/**
 * Global service registry
 */
struct srv_registry {
    struct k_mutex lock;
    struct srv_entry *services;           /* Linked list of services */
    int num_services;
};
```

### Connection Context

```c
/**
 * Context for an open /srv/xxx file
 * Represents an active connection to a service
 */
struct srv_connection {
    struct srv_entry *entry;              /* Which service */
    struct ninep_client *client;          /* 9P client for this connection */
    bool connected;
};
```

## API

### Server-Side: Posting Services

```c
/**
 * Post an in-process 9P server to /srv
 * 
 * @param name    Service name (will appear as /srv/{name})
 * @param server  In-process 9P server
 * @return        0 on success, negative error code on failure
 * 
 * Example:
 *   struct ninep_server *display = ninep_server_register("draw", &draw_ops, drv);
 *   srv_post("display", display);
 *   // Now visible as /srv/display
 */
int srv_post(const char *name, struct ninep_server *server);

/**
 * Post a network service to /srv
 * Creates a service entry that clients can use to connect
 * 
 * @param name       Service name
 * @param transport  Transport type ("tcp", "ble", "uart", "thread")
 * @param address    Transport-specific address
 * @return           0 on success, negative error code on failure
 * 
 * Example:
 *   // Post a remote service
 *   srv_post_network("remote_sensor", "tcp", "192.168.1.100:564");
 *   // Now clients can: mount /srv/remote_sensor /remote
 */
int srv_post_network(const char *name, const char *transport, const char *address);

/**
 * Remove a service from /srv
 * 
 * @param name  Service name to remove
 * @return      0 on success, negative error code on failure
 */
int srv_remove(const char *name);

/**
 * List all services in /srv
 * 
 * @param callback  Called for each service
 * @param user_data User data passed to callback
 */
void srv_foreach(void (*callback)(const struct srv_entry *, void *), void *user_data);
```

### Client-Side: Using Services

```c
/**
 * Open a connection to a service via /srv
 * This is handled by the ns_open() API - opening /srv/xxx
 * automatically establishes a connection
 * 
 * Example:
 *   int fd = ns_open("/srv/sensors", FS_O_RDWR);
 *   // fd is now a connection to the sensors service
 *   
 *   // Mount it in namespace
 *   ns_mount_fd(fd, "/remote/sensors", 0);
 */

/**
 * Mount a service from /srv into the namespace
 * Convenience function that combines open + mount
 * 
 * @param srv_name   Service name (without /srv/ prefix)
 * @param mnt_point  Where to mount in namespace
 * @param flags      Namespace flags
 * @return           0 on success, negative error code on failure
 * 
 * Example:
 *   // Instead of:
 *   //   int fd = ns_open("/srv/sensors", FS_O_RDWR);
 *   //   ns_mount_fd(fd, "/remote/sensors", 0);
 *   
 *   // Just do:
 *   srv_mount("sensors", "/remote/sensors", 0);
 */
int srv_mount(const char *srv_name, const char *mnt_point, uint32_t flags);
```

## Implementation

### `/srv` as Synthetic Filesystem

```c
/**
 * 9P operations for /srv filesystem
 */

static int srv_open(void *priv, uint32_t fid, uint8_t mode,
                    struct ninep_qid *qid, uint32_t *iounit) {
    struct srv_registry *reg = priv;
    struct srv_entry *entry = /* lookup entry by fid */;
    
    if (!entry) {
        return -ENOENT;
    }
    
    /* Create connection context */
    struct srv_connection *conn = k_malloc(sizeof(*conn));
    conn->entry = entry;
    
    /* Establish connection based on service type */
    switch (entry->type) {
    case SRV_TYPE_LOCAL:
        /* Create client connected to in-process server */
        conn->client = ninep_client_create_local(entry->local.server);
        break;
        
    case SRV_TYPE_NETWORK:
        /* Create client with network connection */
        conn->client = ninep_client_create();
        int rc = ninep_connect(conn->client, 
                              entry->network.transport,
                              entry->network.address);
        if (rc < 0) {
            k_free(conn);
            return rc;
        }
        break;
        
    case SRV_TYPE_IPC:
        /* Create client using message queue */
        conn->client = ninep_client_create_ipc(entry->ipc.msgq);
        break;
    }
    
    conn->connected = true;
    
    /* Store connection in FID table */
    /* ... */
    
    return 0;
}

static int srv_read(void *priv, uint32_t fid, uint64_t offset,
                    uint32_t count, void *data, uint32_t *nread) {
    /* Reading from /srv/xxx returns nothing - it's just a connection handle */
    *nread = 0;
    return 0;
}

static int srv_readdir(void *priv, uint32_t fid, uint64_t offset,
                       uint32_t count, void *data, uint32_t *nread) {
    struct srv_registry *reg = priv;
    
    /* List all registered services */
    k_mutex_lock(&reg->lock, K_FOREVER);
    
    int n = 0;
    for (struct srv_entry *e = reg->services; e; e = e->next) {
        /* Format directory entry */
        struct ninep_stat stat = {
            .name = e->name,
            .qid.type = QTFILE,
            /* ... */
        };
        /* Append to data buffer */
        /* ... */
    }
    
    k_mutex_unlock(&reg->lock);
    
    *nread = n;
    return 0;
}

static const struct ninep_server_ops srv_ops = {
    .open = srv_open,
    .read = srv_read,
    .readdir = srv_readdir,
    /* ... */
};

/**
 * Initialize /srv filesystem
 */
int srv_init(void) {
    struct srv_registry *reg = k_malloc(sizeof(*reg));
    k_mutex_init(&reg->lock);
    reg->services = NULL;
    reg->num_services = 0;
    
    /* Register /srv as synthetic filesystem */
    struct ninep_server *srv_fs = ninep_server_register("srv", &srv_ops, reg);
    
    /* Mount in default namespace */
    ns_mount_server(srv_fs, "/srv", 0);
    
    return 0;
}
```

## Usage Examples

### Example 1: Local Service Discovery

```c
/* Driver initialization - post display service */
void display_driver_init(void) {
    struct ninep_server *display = ninep_server_register("draw", &draw_ops, drv);
    srv_post("display", display);
    
    printk("Display service posted to /srv/display\n");
}

/* Application - discover and use display */
void app_thread(void) {
    /* Service discovery - list what's available */
    int dir = ns_opendir("/srv");
    struct fs_dirent entry;
    
    printk("Available services:\n");
    while (ns_readdir(dir, &entry) == 0) {
        printk("  /srv/%s\n", entry.name);
    }
    ns_closedir(dir);
    
    /* Mount display service */
    srv_mount("display", "/dev/draw", 0);
    
    /* Now use it */
    int ctl = ns_open("/dev/draw/ctl", FS_O_WRITE);
    ns_write(ctl, "rect 10 10 100 100", 18);
    ns_close(ctl);
}
```

### Example 2: Network Service Registry

```c
/* Service discovery thread */
void discover_services(void) {
    /* Scan local network for services */
    discover_mdns_services();  // Use mDNS/Zeroconf
    
    /* Post discovered services to /srv */
    srv_post_network("node1_sensors", "tcp", "192.168.1.100:564");
    srv_post_network("node2_sensors", "tcp", "192.168.1.101:564");
    srv_post_network("cloud", "tcp", "cloud.example.com:564");
}

/* Application - use any available sensor */
void read_temperature(void) {
    /* Try each sensor service until one works */
    const char *services[] = {
        "local_sensors",
        "node1_sensors", 
        "node2_sensors",
        NULL
    };
    
    for (int i = 0; services[i]; i++) {
        if (srv_mount(services[i], "/tmp/sensors", NS_FLAG_REPLACE) == 0) {
            int fd = ns_open("/tmp/sensors/temp", FS_O_READ);
            if (fd >= 0) {
                char buf[32];
                ns_read(fd, buf, sizeof(buf));
                printk("Temperature: %s\n", buf);
                ns_close(fd);
                return;
            }
        }
    }
    
    printk("No temperature sensors available\n");
}
```

### Example 3: Dynamic Service Management

```c
/* Service manager thread */
void service_manager(void) {
    while (1) {
        /* Check health of registered services */
        srv_foreach(check_service_health, NULL);
        
        /* Remove dead services */
        if (is_service_dead("remote_sensor")) {
            srv_remove("remote_sensor");
        }
        
        /* Discover new services */
        discover_new_services();
        
        k_sleep(K_SECONDS(10));
    }
}

void check_service_health(const struct srv_entry *entry, void *user_data) {
    /* Try to connect and ping */
    int fd = ns_open("/srv/{entry->name}", FS_O_RDWR);
    if (fd < 0) {
        printk("Service %s is dead\n", entry->name);
        srv_remove(entry->name);
    } else {
        ns_close(fd);
    }
}
```

### Example 4: Service Aliases

```c
/* Create convenient aliases via union mounts */
void setup_aliases(void) {
    /* Post all sensor services */
    srv_post("sensor_local", local_sensor_srv);
    srv_post_network("sensor_node1", "tcp", "192.168.1.100:564");
    srv_post_network("sensor_node2", "tcp", "192.168.1.101:564");
    
    /* Mount them all in /sensors with union */
    srv_mount("sensor_local", "/sensors", 0);
    srv_mount("sensor_node1", "/sensors", NS_FLAG_AFTER);
    srv_mount("sensor_node2", "/sensors", NS_FLAG_AFTER);
    
    /* Now /sensors shows all sensors from all services! */
    /* Reading /sensors/temp tries local first, then node1, then node2 */
}
```

### Example 5: Service Import/Export

```c
/* Export a local service over network */
void export_service(const char *srv_name, const char *address) {
    /* Get the service */
    int srv_fd = ns_open("/srv/{srv_name}", FS_O_RDWR);
    
    /* Start 9P server that forwards to this service */
    struct ninep_server *export_srv = create_export_server(srv_fd);
    
    /* Listen on network */
    ninep_server_listen(export_srv, "tcp", address);
    
    printk("Service %s exported on %s\n", srv_name, address);
}

/* Import a remote service */
void import_service(const char *remote_addr, const char *local_name) {
    /* Post network service */
    srv_post_network(local_name, "tcp", remote_addr);
    
    printk("Service %s imported as /srv/%s\n", remote_addr, local_name);
}

/* Example usage */
void main(void) {
    /* Export our display service */
    export_service("display", "0.0.0.0:9564");
    
    /* Import remote sensor service */
    import_service("192.168.1.100:564", "remote_sensors");
    
    /* Now other devices can access our display, and we can access their sensors */
}
```

## Integration with namespace system

The `/srv` filesystem integrates seamlessly with the namespace system:

```c
/* Initialize system */
void system_init(void) {
    /* Initialize namespace subsystem */
    ns_init();
    
    /* Initialize /srv */
    srv_init();  // Mounts /srv in default namespace
    
    /* Now /srv is available in all namespaces by default */
}

/* Per-thread /srv customization */
void custom_namespace_thread(void) {
    /* This thread inherits /srv from parent */
    
    /* Can add thread-local services */
    srv_post("thread_local_service", my_srv);
    
    /* Or replace /srv entirely */
    struct ninep_server *custom_srv = create_custom_srv_registry();
    ns_mount_server(custom_srv, "/srv", NS_FLAG_REPLACE);
}
```

## Service Discovery Protocols

### mDNS/Zeroconf Integration

```c
/**
 * Advertise services via mDNS
 */
void srv_advertise_mdns(const char *srv_name) {
    struct srv_entry *entry = srv_lookup(srv_name);
    
    if (entry->type == SRV_TYPE_LOCAL) {
        /* Advertise local service on network */
        mdns_advertise("_9p._tcp", srv_name, 564);
    }
}

/**
 * Discover services via mDNS and post to /srv
 */
void srv_discover_mdns(void) {
    mdns_browse("_9p._tcp", mdns_callback, NULL);
}

void mdns_callback(const char *name, const char *host, uint16_t port, void *user_data) {
    char addr[128];
    snprintf(addr, sizeof(addr), "%s:%d", host, port);
    
    srv_post_network(name, "tcp", addr);
    
    printk("Discovered service: /srv/%s -> %s\n", name, addr);
}
```

### CoAP Resource Directory Integration

For constrained networks (6LoWPAN, Thread), use CoAP-RD:

```c
/**
 * Register services with CoAP Resource Directory
 */
void srv_register_coap_rd(const char *rd_addr) {
    /* For each local service, register with RD */
    srv_foreach(register_with_rd, rd_addr);
}

void register_with_rd(const struct srv_entry *entry, void *user_data) {
    const char *rd_addr = user_data;
    
    /* POST to rd_addr with service metadata */
    coap_post(rd_addr, "9p-service", entry->name, "tcp", my_addr);
}

/**
 * Discover services from CoAP RD and post to /srv
 */
void srv_discover_coap_rd(const char *rd_addr) {
    /* GET from rd_addr */
    struct coap_response *resp = coap_get(rd_addr);
    
    /* Parse response and post services */
    parse_rd_response(resp, post_discovered_service, NULL);
}
```

## Benefits

### 1. Service Discovery
Applications don't need hardcoded addresses:

```c
// Instead of hardcoding:
ninep_connect(client, "tcp", "192.168.1.100:564");

// Just mount from /srv:
srv_mount("sensors", "/remote/sensors", 0);
```

### 2. Dynamic Binding
Services can come and go at runtime:

```c
/* USB sensor plugged in */
void usb_added(struct usb_device *dev) {
    struct ninep_server *srv = create_usb_sensor_server(dev);
    srv_post("usb_sensor", srv);
    // Immediately visible as /srv/usb_sensor
}

/* USB sensor removed */
void usb_removed(struct usb_device *dev) {
    srv_remove("usb_sensor");
}
```

### 3. Network Transparency
Local and remote services are indistinguishable:

```c
/* Doesn't matter if service is local or remote */
srv_mount("display", "/dev/draw", 0);

/* Could be:
 *   - Local in-process driver
 *   - Service on another thread
 *   - Service on another device over TCP
 *   - Service over BLE
 *   All work the same!
 */
```

### 4. Service Composition
Services can be stacked and composed:

```c
/* Cache layer */
srv_post("cache", cache_srv);
srv_mount("cache", "/tmp/cache", 0);

/* Backend service */
srv_mount("remote_storage", "/tmp/backend", 0);

/* Cache forwards to backend */
configure_cache("/tmp/cache", "/tmp/backend");

/* Applications use cache */
srv_mount("cache", "/storage", 0);
```

## Configuration

```kconfig
config SRV_REGISTRY
    bool "Service registry (/srv)"
    depends on NAMESPACE
    default y
    help
      Enable the /srv service registry filesystem.
      Services can post themselves to /srv for discovery and connection.

config SRV_MAX_SERVICES
    int "Maximum number of services in /srv"
    depends on SRV_REGISTRY
    default 32
    help
      Maximum number of services that can be registered in /srv.

config SRV_MAX_NAME_LEN
    int "Maximum service name length"
    depends on SRV_REGISTRY
    default 64
    help
      Maximum length of a service name in /srv.

config SRV_AUTO_MDNS
    bool "Automatically advertise services via mDNS"
    depends on SRV_REGISTRY && MDNS_RESPONDER
    default y
    help
      Automatically advertise local services via mDNS when posted to /srv.

config SRV_AUTO_COAP_RD
    bool "Automatically register with CoAP Resource Directory"
    depends on SRV_REGISTRY && COAP
    default n
    help
      Automatically register services with CoAP RD when posted to /srv.
      Set the RD address via srv_set_coap_rd().
```

## Summary

The `/srv` service registry is a critical piece of Plan 9's architecture that enables:

✅ **Service discovery** - Find available services dynamically
✅ **Service rendezvous** - Connect clients to servers  
✅ **Dynamic binding** - Services come and go at runtime
✅ **Network transparency** - Local and remote services identical
✅ **Service composition** - Stack and compose services

Implementation:
- `/srv` is a synthetic filesystem
- Opening `/srv/xxx` establishes connection to service `xxx`
- Services post themselves via `srv_post()` 
- Clients discover via directory listing or `srv_mount()`
- Integrates with mDNS/CoAP-RD for network discovery

This completes the Plan 9 model - now you have **everything**:
1. Per-thread namespaces ✅
2. Union mounts ✅
3. 9P network transparency ✅
4. Drivers as filesystems ✅
5. **Service registry (/srv)** ✅

The system is now truly Plan 9-like!
