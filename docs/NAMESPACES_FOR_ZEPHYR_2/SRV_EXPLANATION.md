# The Role of /srv in Plan 9 and Zephyr

## Your Question

> "what role does the '/srv' typically play in plan 9? service registry? how would that be mapped into Zephyr? is it captured effectively here?"

## Short Answer

**Role in Plan 9:** `/srv` is the **service registry and rendezvous point** where 9P servers post themselves so clients can discover and connect to them.

**Mapping to Zephyr:** Implemented as a synthetic filesystem where services register themselves. Opening `/srv/{name}` establishes a connection to that service.

**Was it captured?** **No, not originally** - the original spec mentioned "synthetic filesystems" but didn't explain `/srv`. I've now added a complete addendum: [SRV_REGISTRY_ADDENDUM.md](./SRV_REGISTRY_ADDENDUM.md)

## Long Answer

### What is /srv?

In Plan 9, `/srv` solves the **service discovery problem**:

**Without /srv:**
```c
// How do I know where the display driver is?
// Hardcoded addresses? Configuration files? Magic?
```

**With /srv:**
```bash
# Driver posts itself
% bind '#I' /srv/draw

# Client discovers it
% ls /srv
draw
mouse
cons
...

# Client connects
% mount /srv/draw /dev/draw
```

`/srv` is a **directory of named connections**. Each "file" in `/srv` represents a connection to a service. Opening that file gives you a file descriptor you can then mount.

### The Plan 9 Pattern

```
1. Server starts up
2. Server posts itself to /srv with a name
3. Clients list /srv to discover available services  
4. Clients open /srv/{service} to get a connection
5. Clients mount that connection wherever they want
```

This is **fundamental** to Plan 9's architecture. It's how:
- Services advertise themselves
- Clients find services
- Dynamic binding works
- Network transparency is achieved

### How it Maps to Zephyr

In our Zephyr implementation:

```c
/* Server side - post a service */
void display_driver_init(void) {
    struct ninep_server *srv = ninep_server_register("draw", &draw_ops, drv);
    srv_post("display", srv);  // Now visible as /srv/display
}

/* Client side - discover and connect */
void application_thread(void) {
    // List available services
    int dir = ns_opendir("/srv");
    struct fs_dirent entry;
    while (ns_readdir(dir, &entry) == 0) {
        printk("Found: /srv/%s\n", entry.name);
    }
    ns_closedir(dir);
    
    // Connect to service
    srv_mount("display", "/dev/draw", 0);
    
    // Use it
    int fd = ns_open("/dev/draw/ctl", FS_O_WRITE);
    ns_write(fd, "clear", 5);
    ns_close(fd);
}
```

### Why /srv is Critical

Without `/srv`, you're missing a **key piece** of the Plan 9 model:

❌ **Without /srv:**
- Hardcoded service locations
- Manual service discovery
- No dynamic binding
- Services can't advertise themselves

✅ **With /srv:**
- Services self-register
- Automatic service discovery
- Dynamic binding (services come and go)
- Network transparency (local and remote services identical)

### Real-World Example: IoT Gateway

**Scenario:** Gateway aggregating sensor data from multiple mesh nodes

**Without /srv:**
```c
// Configuration nightmare
struct ninep_client *node1 = ninep_client_create();
ninep_connect(node1, "tcp", "192.168.1.100:564");  // Hardcoded!

struct ninep_client *node2 = ninep_client_create();
ninep_connect(node2, "tcp", "192.168.1.101:564");  // Hardcoded!

// What if a node goes offline? What if a new node appears?
```

**With /srv:**
```c
/* Discovery thread automatically posts services */
void discovery_thread(void) {
    while (1) {
        scan_network_for_9p_services();  // Uses mDNS
        k_sleep(K_SECONDS(30));
    }
}

void scan_network_for_9p_services(void) {
    // Found node via mDNS
    srv_post_network("node1_sensors", "tcp", "192.168.1.100:564");
    srv_post_network("node2_sensors", "tcp", "192.168.1.101:564");
}

/* Application just uses whatever is available */
void read_all_sensors(void) {
    int dir = ns_opendir("/srv");
    struct fs_dirent entry;
    
    // For each service
    while (ns_readdir(dir, &entry) == 0) {
        if (strstr(entry.name, "sensors")) {
            // Mount it temporarily
            char mnt[64];
            snprintf(mnt, sizeof(mnt), "/tmp/%s", entry.name);
            srv_mount(entry.name, mnt, 0);
            
            // Read temperature
            char path[128];
            snprintf(path, sizeof(path), "%s/temp", mnt);
            int fd = ns_open(path, FS_O_READ);
            if (fd >= 0) {
                char buf[32];
                ns_read(fd, buf, sizeof(buf));
                printk("Node %s temp: %s\n", entry.name, buf);
                ns_close(fd);
            }
            
            ns_unmount(mnt, NULL);
        }
    }
    ns_closedir(dir);
}
```

Services come and go automatically. Application adapts. **That's the power of /srv.**

### Integration with Network Discovery

`/srv` becomes even more powerful when integrated with discovery protocols:

**mDNS (for local network):**
```c
/* Advertise local services */
void srv_init(void) {
    // When service posted to /srv, advertise via mDNS
    srv_set_advertise_callback(advertise_mdns);
}

void advertise_mdns(const char *name) {
    mdns_advertise("_9p._tcp", name, 564);
}

/* Discover remote services */
void srv_discover(void) {
    mdns_browse("_9p._tcp", on_service_found, NULL);
}

void on_service_found(const char *name, const char *addr, uint16_t port, void *ctx) {
    char address[128];
    snprintf(address, sizeof(address), "%s:%d", addr, port);
    srv_post_network(name, "tcp", address);
    
    // Now visible as /srv/{name}
}
```

**CoAP Resource Directory (for constrained networks):**
```c
/* Register with CoAP RD */
void srv_register_with_rd(const char *rd_url) {
    srv_foreach(post_to_rd, rd_url);
}

/* Query RD for services */
void srv_discover_from_rd(const char *rd_url) {
    coap_get(rd_url, on_rd_response, NULL);
}
```

### The Complete Picture

With `/srv` added, you now have **all five pillars** of Plan 9:

1. ✅ **Per-process namespaces** - Each thread customizes its view
2. ✅ **Union mounts** - Multiple sources at one path
3. ✅ **9P protocol** - Network-transparent resource access
4. ✅ **Everything-is-a-file** - Drivers expose as filesystems
5. ✅ **Service registry (/srv)** - Discovery and rendezvous

This is a **complete** Plan 9-style system for embedded devices!

### Was it Captured?

**Original spec:** ❌ No - mentioned "synthetic filesystems" but didn't explain `/srv`

**With addendum:** ✅ Yes - complete specification including:
- Data structures
- API (`srv_post()`, `srv_mount()`, etc.)
- Implementation details
- Integration with mDNS/CoAP-RD
- Usage examples
- Benefits and rationale

## Summary

`/srv` is **not optional** if you want a true Plan 9-style system. It's the mechanism that enables:

- **Service discovery** - "What services are available?"
- **Dynamic binding** - Services appear and disappear at runtime
- **Network transparency** - Local and remote services indistinguishable
- **Decoupling** - Clients don't need to know where services are

The [SRV_REGISTRY_ADDENDUM.md](./SRV_REGISTRY_ADDENDUM.md) now provides a complete specification for implementing `/srv` in Zephyr, making the system truly Plan 9-like.

**Great catch!** You identified a critical missing piece. The spec is now complete. 🎯
