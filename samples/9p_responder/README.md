# 9P Responder Sample

A simple 9P protocol responder that can communicate with Plan 9 port tools.

## What It Does

Responds to basic 9P messages:
- **Tversion/Rversion** - Protocol version negotiation
- **Tattach/Rattach** - Filesystem attachment
- **Tclunk/Rclunk** - Resource cleanup

## Building

```bash
cd ~/zephyr-workspaces/9p4z-workspace/9p4z
source activate.sh
west build -b qemu_x86 9p4z/samples/9p_responder -p
```

## Running with QEMU

### Option 1: Unix Socket (Easiest)

```bash
# Run QEMU with serial on Unix socket
west build -t run -- -serial unix:/tmp/9p.sock,server -display none

# In another terminal, connect with a 9P client tool
# (You'll need to write a small C tool to send 9P messages over the socket)
```

### Option 2: PTY (Most Flexible)

```bash
# Run QEMU - it will print the PTY path
west build -t run -- -serial pty -display none
# Note the output: "char device redirected to /dev/pts/X"

# In another terminal, use a 9P tool or write a small C client
```

## Testing with a C Client

Create a simple test client (`test_client.c`):

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

/* Write uint32 little-endian */
void put_u32(uint8_t *buf, size_t *off, uint32_t val) {
    buf[(*off)++] = val & 0xff;
    buf[(*off)++] = (val >> 8) & 0xff;
    buf[(*off)++] = (val >> 16) & 0xff;
    buf[(*off)++] = (val >> 24) & 0xff;
}

/* Write uint16 little-endian */
void put_u16(uint8_t *buf, size_t *off, uint16_t val) {
    buf[(*off)++] = val & 0xff;
    buf[(*off)++] = (val >> 8) & 0xff;
}

/* Write string (2-byte length + data) */
void put_string(uint8_t *buf, size_t *off, const char *str, uint16_t len) {
    put_u16(buf, off, len);
    memcpy(&buf[*off], str, len);
    *off += len;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <unix_socket or pty>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    /* Build Tversion message */
    uint8_t msg[64];
    size_t off = 0;

    /* Header will be filled after */
    off = 7;

    /* msize */
    put_u32(msg, &off, 8192);

    /* version */
    put_string(msg, &off, "9P2000", 6);

    /* Now fill header */
    size_t total_size = off;
    off = 0;
    put_u32(msg, &off, total_size);  /* size */
    msg[off++] = 100;                 /* type = Tversion */
    put_u16(msg, &off, 0xFFFF);      /* tag = NOTAG */

    printf("Sending Tversion (%zu bytes)\\n", total_size);
    write(fd, msg, total_size);

    /* Read response */
    uint8_t resp[64];
    ssize_t n = read(fd, resp, sizeof(resp));
    if (n > 0) {
        printf("Received %zd bytes\\n", n);
        printf("Response: size=%u, type=%u\\n",
               *(uint32_t*)resp, resp[4]);
    }

    close(fd);
    return 0;
}
```

Compile and run:
```bash
gcc -o test_client test_client.c
./test_client /tmp/9p.sock  # or /dev/pts/X
```

## Expected Output

QEMU console should show:
```
=== 9P Responder Sample ===
9P responder ready - waiting for connections...
Received message: 19 bytes
Message: type=100, tag=65535, size=19
Client: msize=8192, version=9P2000
Sent Rversion: msize=8192, version=9P2000
```

## Limitations

This is a minimal responder for demonstration:
- Only handles Tversion, Tattach, Tclunk
- No actual filesystem backend
- No Twalk/Topen/Tread/Twrite support yet
- No error messages (Rerror)

## Next Steps

To make this a full 9P server, you'd need to add:
1. Fid tracking (use the fid management API)
2. Filesystem operations (Twalk, Topen, Tread, Twrite)
3. Error handling (Rerror responses)
4. Stat support
