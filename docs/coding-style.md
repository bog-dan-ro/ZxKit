# C++ coding style

ZxKit uses [Qt's coding style](https://wiki.qt.io/Qt_Coding_Style) as its formatting
baseline, encoded in the root `.clang-format`. The implementation remains standard
C++23. Existing ownership and public API conventions in [AGENTS.md](../AGENTS.md)
continue to apply.

Use four spaces, one statement/declaration per line, spaces around binary
operators, and a space after control-flow keywords. Class and function opening
braces go on a new line; control-flow and lambda braces stay attached. Use
camelCase for functions/variables, PascalCase for types, and descriptive names.

```cpp
using namespace std::literals;

std::string mediaName(std::string_view extension)
{
    if (extension == ".tap"sv || extension == ".tzx"sv)
        return "tape"s;
    return "disk"s;
}
```

`"text"s` creates an owning string; `"text"sv` creates a view. Scope the literal
namespace to implementation files or function bodies, never an entire public
header. Include `<string>` and `<string_view>` directly as needed. Keep raw C
literals for APIs requiring null-terminated pointers or compile-time format
strings. Binary views use `sv` so embedded NUL bytes preserve their length.
Formatting cannot enforce these ownership rules, so check literals during review.

Format new files and check them with:

```sh
clang-format -i path/to/new.cpp path/to/new.h
clang-format --dry-run --Werror path/to/new.cpp path/to/new.h
```

For existing files, format only the changed ranges, for example
`clang-format -i --lines=120:155 path/to/existing.cpp`. Keep unrelated code and
working-tree edits intact. Formatting changes still require the complete unit
suite under the repository's verification rules.
