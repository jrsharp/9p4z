# 9P for Zephyr - Testing Guide

This directory contains the comprehensive test suite for the 9p4z project, implementing best practices from the Zephyr Test Framework (ztest).

## Test Organization

### Test Suites

| Suite | Type | Description | File |
|-------|------|-------------|------|
| `ninep_protocol` | Unit | Protocol message parsing and serialization | `protocol_test.c` |
| `ninep_fid` | Unit | File ID (fid) management | `fid_test.c` |
| `ninep_tag` | Unit | Request tag management | `tag_test.c` |
| `ninep_transport` | Integration | Transport layer abstraction | `transport_test.c` |

## Test Structure

All tests follow the Zephyr ztest framework conventions:

```c
/* Test fixture setup (runs once before all tests in suite) */
static void *suite_setup(void) {
    // Initialize shared resources
    return fixture_data;
}

/* Per-test setup (runs before each test) */
static void test_before(void *f) {
    // Reset state before each test
}

/* Per-test teardown (runs after each test) */
static void test_after(void *f) {
    // Cleanup after each test
}

/* Define test suite */
ZTEST_SUITE(suite_name, NULL, suite_setup, test_before, test_after, NULL);

/* Individual test case */
ZTEST(suite_name, test_case_name) {
    // Test implementation
}
```

## Running Tests

### Quick Start

```bash
# Run all tests on native_posix
./scripts/run_tests.sh

# Run on specific platform
./scripts/run_tests.sh qemu_x86

# Verbose output
./scripts/run_tests.sh native_posix -v
```

### Using West Directly

```bash
# Run all tests
west twister -T tests/

# Run on specific platform
west twister -p native_posix -T tests/

# Run specific test suite
west twister -p native_posix -T tests/ -s libraries.ninep.protocol

# Run with inline logs (see output as tests run)
west twister -p native_posix -T tests/ --inline-logs

# Run with coverage
west twister -p native_posix -T tests/ --coverage --coverage-tool=gcovr
```

### Filtering by Tags

```bash
# Run only unit tests
west twister -p native_posix -T tests/ --tag unit

# Run only integration tests
west twister -p native_posix -T tests/ --tag integration

# Run protocol tests only
west twister -p native_posix -T tests/ --tag protocol
```

## Test Coverage

### Protocol Tests (protocol_test.c)

**Basic Functionality:**
- ✅ Header parsing (little-endian)
- ✅ Header serialization
- ✅ String parsing with length prefix
- ✅ String serialization
- ✅ Qid parsing (13 bytes: type, version, path)
- ✅ Qid serialization

**Error Handling:**
- ✅ Buffer too small
- ✅ NULL parameter validation
- ✅ Invalid message size
- ✅ String overflow detection
- ✅ Empty string handling

**Roundtrip Tests:**
- ✅ Header write → read consistency
- ✅ Qid write → read consistency
- ✅ Large value handling (64-bit paths)

### Fid Tests (fid_test.c)

**Basic Operations:**
- ✅ Fid allocation
- ✅ Fid lookup
- ✅ Fid free
- ✅ Duplicate prevention

**Resource Management:**
- ✅ Table exhaustion handling
- ✅ Fid reuse after free
- ✅ User data attachment
- ✅ Qid storage in fid

**Edge Cases:**
- ✅ Maximum concurrent fids (CONFIG_NINEP_MAX_FIDS)
- ✅ Free + reallocate same fid number
- ✅ Lookup non-existent fid

### Tag Tests (tag_test.c)

**Basic Operations:**
- ✅ Tag allocation
- ✅ Tag lookup
- ✅ Tag free
- ✅ Sequential allocation uniqueness

**Resource Management:**
- ✅ Table exhaustion (CONFIG_NINEP_MAX_TAGS)
- ✅ Tag reuse after free
- ✅ User data attachment
- ✅ Free non-existent tag

### Transport Tests (transport_test.c)

**Mock Transport:**
- ✅ Send operation
- ✅ Error handling on send
- ✅ Start/stop lifecycle
- ✅ Receive callback invocation

**Integration:**
- ✅ Send header via transport
- ✅ NULL parameter checks
- ✅ Full message roundtrip
- ✅ Header parsing from received data

## Continuous Integration

The project uses GitHub Actions for automated testing. See `.github/workflows/tests.yml`.

### CI/CD Pipeline

1. **Unit Tests** - Run on multiple platforms:
   - native_posix
   - qemu_x86

2. **Sample Builds** - Verify samples build correctly:
   - uart_echo on native_posix, qemu_x86, qemu_cortex_m3

3. **Coverage Reports** - Generate code coverage with gcovr

### Triggering CI

- Push to `main` or `develop` branches
- Pull requests targeting `main` or `develop`

## Test Configuration

### testcase.yaml

The `testcase.yaml` file defines test metadata for Twister:

```yaml
common:
  tags: ninep                    # Tag all tests with 'ninep'
  platform_allow: ...            # Allowed platforms
  integration_platforms: [...]   # Platforms for integration tests
  harness: ztest                 # Use ztest framework

tests:
  libraries.ninep.protocol:
    tags: ninep protocol unit    # Specific tags
    min_ram: 32                  # Minimum RAM requirement
```

## Writing New Tests

### Guidelines

1. **Use Fixtures**: Set up common test data in suite_setup()
2. **Reset State**: Implement test_before() to ensure test isolation
3. **Test Error Cases**: Always test NULL parameters and boundary conditions
4. **Descriptive Names**: Use clear test names like `test_fid_exhaustion`
5. **Assertions**: Use appropriate zassert_* macros:
   - `zassert_equal(a, b, msg)` - Values must be equal
   - `zassert_not_equal(a, b, msg)` - Values must differ
   - `zassert_true(cond, msg)` - Condition must be true
   - `zassert_not_null(ptr, msg)` - Pointer must not be NULL
   - `zassert_mem_equal(a, b, size, msg)` - Memory regions must match

### Example Test

```c
ZTEST(ninep_protocol, test_roundtrip_example)
{
    uint8_t buf[7];
    struct ninep_msg_header hdr_out = {
        .size = 100,
        .type = NINEP_TWALK,
        .tag = 5,
    };
    struct ninep_msg_header hdr_in;

    /* Write header */
    int ret = ninep_write_header(buf, sizeof(buf), &hdr_out);
    zassert_equal(ret, 7, "Failed to write header");

    /* Read it back */
    ret = ninep_parse_header(buf, sizeof(buf), &hdr_in);
    zassert_equal(ret, 0, "Failed to parse header");

    /* Verify roundtrip consistency */
    zassert_equal(hdr_in.size, hdr_out.size, "Size mismatch");
    zassert_equal(hdr_in.type, hdr_out.type, "Type mismatch");
    zassert_equal(hdr_in.tag, hdr_out.tag, "Tag mismatch");
}
```

## Troubleshooting

### Common Issues

**Test fails on hardware but passes on native_posix:**
- Check endianness assumptions
- Verify buffer sizes are adequate
- Look for timing-related issues

**Twister can't find tests:**
- Ensure `testcase.yaml` is present
- Verify test directory structure
- Check YAML syntax

**Build errors:**
- Ensure all required Kconfig options are set in `prj.conf`
- Check that `CMakeLists.txt` includes all test files

### Debug Output

Enable debug logging in tests:

```c
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(test_module, LOG_LEVEL_DBG);

ZTEST(suite, test) {
    LOG_DBG("Debug message: value=%d", value);
    // ...
}
```

## Future Test Additions

- [ ] UART transport integration tests (requires UART emulation)
- [ ] TCP transport tests (Phase 3)
- [ ] L2CAP transport tests (Phase 4)
- [ ] Full 9P message handler tests (server/client)
- [ ] Performance benchmarks
- [ ] Fuzz testing for protocol parsing

## References

- [Zephyr Test Framework Documentation](https://docs.zephyrproject.org/latest/develop/test/ztest.html)
- [Twister Test Runner](https://docs.zephyrproject.org/latest/develop/test/twister.html)
- [9P Protocol Specification](https://ericvh.github.io/9p-rfc/rfc9p2000.html)