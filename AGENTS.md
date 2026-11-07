# ZxKit contributor instructions

ZxKit is a standalone C++23 emulation library. Spectacol is a separate consumer,
not a source of required files for ZxKit's build, scripts or unit tests.
Local consumer checkouts such as `tests/spectacol` are development conveniences
only. Never add them to ZxKit's index as files or gitlinks, or commit application
revision pointers in this repository. Application changes belong in the
application's own repository.

## C++ conventions

- Follow the repository `.clang-format`, based on Qt's coding style. Use four
  spaces, spaces around binary operators (`a && b`, `value = count + 1`), and
  a space after control-flow keywords (`if (...)`). Function and class opening
  braces go on the next line; control-flow and lambda braces stay attached.
  Put separate statements and variable declarations on separate lines.
- Use descriptive camelCase names for variables/functions and PascalCase for
  types. Single-letter names are for obvious counters or short-lived temporaries.
- Format new or changed C/C++ code with `clang-format -i path/to/file.cpp`.
  Check it with `clang-format --dry-run --Werror path/to/file.cpp`. Avoid
  reformatting unrelated code in an existing file; format changed ranges there.
- Formatting does not enforce string ownership: check the literal rules below
  explicitly. The project remains standard C++23; Qt types and Qt literals are
  not required by this formatting style.
- Prefer `"text"s` for owning `std::string` literals and `"text"sv` for
  `std::string_view` literals, with `using namespace std::literals;` scoped to
  implementation files or function bodies. Include `<string>` or `<string_view>`
  directly as needed. Do not add namespace-wide using directives to public headers.
- Keep C string literals where an API requires a null-terminated character pointer.
  Use `sv` for binary string-view literals so embedded NUL bytes retain their length.

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
