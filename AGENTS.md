# ZxKit contributor instructions

ZxKit is a standalone C++23 emulation library. Spectacol is a separate consumer,
not a source of required files for ZxKit's build, scripts or unit tests.

## Mandatory verification after every change

- After every change, build ZxKit and run the complete unit test suite before
  committing or reporting completion. Do not substitute selected tests or smoke
  runs for the full suite. Documentation and script changes also require a full
  unit test run; an existing current build can be used for documentation edits.
- For emulation-core changes, build and run the full suite in both Debug and
  Release. Keep test assertions enabled in both configurations.
- Fix new failures before committing. Existing known hardware failures must
  remain explicitly documented; do not silently skip tests to obtain a pass.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DZXKIT_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For Release verification, repeat with `build-release` and
`-DCMAKE_BUILD_TYPE=Release`.

## Files and scripts

- Keep required test media, reference data and ROMs in `tests/fixtures`, with
  original notices and attribution. Tests must run without a Spectacol checkout
  or the developer's downloaded test collection.
- Resolve script defaults relative to this repository. Do not hard-code paths
  into a developer home directory or external checkout. External media and ROMs
  may be supported through explicit command-line inputs.
- Keep generated reports and screenshots under the build directory by default.
- Preserve unrelated working-tree edits. Commit only when asked.

See `tests/fixtures/spectrum/README.md` for hardware coverage, audit commands and
known remaining failures.
