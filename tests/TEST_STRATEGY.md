# 9p4z Test Strategy

## Test Philosophy

Our test suite follows a **defense-in-depth** approach with multiple layers of testing to ensure robustness and prevent regressions. All tests must pass before merging to main.

## Test Layers

### 1. Unit Tests (Isolation)
**Purpose**: Test individual components in isolation
**Coverage**: Protocol parsing, FID/TAG management, message building

- `protocol_test.c` - Protocol header parsing, qid/stat encoding
- `fid_test.c` - FID table allocation and management
- `tag_test.c` - TAG table allocation and management
- `message_test.c` - Message parsing and validation
- `message_builder_test.c` - Message construction

**Platforms**: native_posix, qemu_x86, qemu_cortex_m3

### 2. Component Integration Tests
**Purpose**: Test component interactions
**Coverage**: Transport layer, message flow

- `transport_test.c` - Transport abstraction interface
- `uart_transport_test.c` - UART transport with emulation

**Platforms**: native_posix (with UART emulation)

### 3. End-to-End Integration Tests
**Purpose**: Test complete client-server workflows
**Coverage**: Full protocol operations

- `client_server_test.c` - Client/server integration via mock transport
  - Version negotiation
  - Attach/walk/open/read/write/stat/clunk
  - File creation and removal
  - Concurrent operations
  - Error handling

**Platforms**: native_posix, qemu_x86

### 4. Stress & Regression Tests
**Purpose**: Test edge cases, limits, and known issues
**Coverage**: Error scenarios, resource limits, performance

- `stress_test.c` - Comprehensive stress testing
  - Large file transfers (4KB+)
  - Deep path traversal
  - Sequential and partial I/O
  - FID exhaustion and recovery
  - Timeout simulation
  - Send error injection
  - Zero-byte operations
  - Repeated operations (regression)

**Platforms**: native_posix, qemu_x86

## Test Execution

### Run All Tests
```bash
west twister -T tests/
```

### Run Specific Test Suite
```bash
west twister -T tests/ -s libraries.ninep.stress
```

### Run with Coverage
```bash
west twister -T tests/ --coverage
```

### Run on Specific Platform
```bash
west twister -p native_posix -T tests/
```

## Test Metrics

Current test coverage:
- **Total test cases**: 40+ tests
- **Total test code**: 3,000+ LOC
- **Test suites**: 9 suites
- **Platforms tested**: 3 (native_posix, qemu_x86, qemu_cortex_m3)

## Adding New Tests

### When to Add Tests

1. **Before fixing a bug** - Add failing test that reproduces the bug
2. **When adding features** - Add tests for new functionality
3. **When edge cases are discovered** - Add regression tests

### Test Naming Convention

```c
ZTEST(suite_name, test_specific_behavior)
```

Examples:
- `ZTEST(stress, test_large_file_read)` - Tests large file reads
- `ZTEST(client_server, test_walk_nested)` - Tests nested path walking

### Test Structure

```c
/* Test: Description of what's being tested */
ZTEST(suite, test_name)
{
    /* Setup */
    int ret = setup_operation();
    zassert_equal(ret, 0, "Setup failed");

    /* Execute */
    ret = operation_under_test();

    /* Verify */
    zassert_equal(ret, expected_value, "Operation failed");

    /* Cleanup */
    cleanup_operation();
}
```

## Regression Tracking

### Known Issues (Fixed)

| Issue | Test | Commit | Description |
|-------|------|--------|-------------|
| *Example* | `test_timeout` | `abc1234` | Timeout not properly handled |

### Critical Scenarios

Tests that must never regress:
1. **FID Management** - `test_fid_exhaustion` - Ensures FID cleanup
2. **Error Recovery** - `test_timeout`, `test_send_error` - Proper error handling
3. **Data Integrity** - `test_large_file_read` - No data corruption
4. **Concurrent Ops** - `test_concurrent_ops` - Multiple FIDs work correctly

## CI Integration

All tests run automatically on:
- Every push to `main` or `develop`
- Every pull request
- Multiple platforms in parallel

See `.github/workflows/tests.yml` for CI configuration.

## Performance Benchmarks

While not strict pass/fail, these tests track performance:
- Large file transfer speed
- Message throughput
- Memory usage under load
- FID allocation overhead

Run with: `west twister -T tests/ --tag stress`

## Test Maintenance

### Monthly Review
- Review test coverage reports
- Update regression test list
- Remove obsolete tests
- Add tests for new edge cases

### Before Release
- Run full test suite on all platforms
- Verify no skipped tests
- Check coverage meets threshold (>80%)
- Manual testing on real hardware for L2CAP/BLE

## Manual Testing Checklist

Some scenarios require manual testing on real hardware:

- [ ] L2CAP transport on nRF52/nRF53 with iOS client
- [ ] TCP transport over actual network
- [ ] Multi-board scenarios (client on one, server on another)
- [ ] Long-running stress test (24h+)
- [ ] Power-cycle robustness

## Contact

For test infrastructure questions:
See `tests/README.md` or file an issue.
