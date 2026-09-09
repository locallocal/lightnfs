#!/usr/bin/env python3
"""Trailing-comment gate: a `//` comment goes on its own line above the code it
describes, never after it on the same line.

    scripts/trailing_comments.py --check [files...]   # list offenders, exit 1 if any
    scripts/trailing_comments.py --fix   [files...]   # move them above (run clang-format after)

Without files every tracked C++ source outside third_party/ is taken (the set
scripts/format_check.sh gates).  Kept where they are, by convention:
  `}  // namespace x` and any other closing-brace-only line, `#if/#else/#endif  // …`,
  `// NOLINT…`, `// clang-format on|off`, and a line continued with a backslash.
A comment after an opening brace (`} else {  // why`) moves inside the block; one
after anything else moves above the line at its indentation, together with the
aligned `//` lines that continue it.
"""
import re
import subprocess
import sys

KEEP_COMMENT = re.compile(r'//\s*(NOLINT|clang-format (on|off))')


def tracked_sources():
    out = subprocess.check_output(
        ['git', 'ls-files', '*.cpp', '*.hpp', ':!:third_party/**', ':!:tests/*.inc'])
    return out.decode().split()


def split(line, state):
    """(code, comment) for `line`; `comment` is the trailing // comment or None.
    `state` carries raw-string and block-comment context across lines."""
    i, n = 0, len(line)
    while i < n:
        if state['raw'] is not None:
            j = line.find(')' + state['raw'] + '"', i)
            if j < 0:
                return line, None
            i = j + len(state['raw']) + 2
            state['raw'] = None
            continue
        if state['block']:
            j = line.find('*/', i)
            if j < 0:
                return line, None
            state['block'] = False
            i = j + 2
            continue
        c = line[i]
        if c == '"':
            if i > 0 and line[i - 1] == 'R':
                m = re.match(r'"([^(]{0,16})\(', line[i:])
                if m:
                    state['raw'] = m.group(1)
                    i += len(m.group(0))
                    continue
            i += 1
            while i < n and line[i] != '"':
                if line[i] == '\\':
                    i += 1
                i += 1
            i += 1
            continue
        if c == "'":
            i += 1
            while i < n and line[i] != "'":
                if line[i] == '\\':
                    i += 1
                i += 1
            i += 1
            continue
        if c == '/' and i + 1 < n and line[i + 1] == '*':
            j = line.find('*/', i + 2)
            if j < 0:
                state['block'] = True
                return line, None
            i = j + 2
            continue
        if c == '/' and i + 1 < n and line[i + 1] == '/':
            return line[:i].rstrip(), line[i:]
        i += 1
    return line, None


def kept(code, comment):
    cs = code.strip()
    if not cs:
        return True  # a comment-only line
    if re.fullmatch(r'}[;)]*', cs):
        return True  # `}  // namespace x`, `};  // struct`
    if re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b', cs):
        return True
    if cs.endswith('\\'):
        return True
    if KEEP_COMMENT.search(comment):
        return True
    return False


def continuation_lines(lines, start, column):
    """Indexes of the lines after `start` that are `//` comments aligned at `column`."""
    out = []
    i = start + 1
    while i < len(lines):
        line = lines[i]
        stripped = line.lstrip()
        if not stripped.startswith('//') or len(line) - len(stripped) != column:
            break
        out.append(i)
        i += 1
    return out


def offenders(lines):
    """[(index, code, comment)] for the lines whose trailing comment must move."""
    found = []
    state = {'raw': None, 'block': False}
    for i, line in enumerate(lines):
        code, comment = split(line, state)
        if comment is None or kept(code, comment):
            continue
        found.append((i, code, comment))
    return found


def tidy_comment(comment):
    """`//text` → `// text`; runs of three or more spaces (table padding) collapse."""
    body = comment[2:]
    if body and not body[0].isspace():
        body = ' ' + body
    body = re.sub(r' {3,}', ' ', body.rstrip())
    return '//' + body


def fix(lines):
    found = offenders(lines)
    if not found:
        return lines, 0
    done = set()
    out = []
    by_index = {i: (code, comment) for i, code, comment in found}
    i = 0
    while i < len(lines):
        if i in by_index and i not in done:
            code, comment = by_index[i]
            column = len(lines[i]) - len(comment)  # where the `//` sits
            tail = continuation_lines(lines, i, column)
            texts = [tidy_comment(comment)] + [tidy_comment(lines[j].strip()) for j in tail]
            indent = re.match(r'\s*', code).group(0)
            cs = code.strip()
            if cs.endswith('{') and cs.startswith('}'):
                # `} else {  // why` → the comment opens the block
                out.append(code)
                inner = indent + '    '
                out.extend(inner + t for t in texts)
            else:
                out.extend(indent + t for t in texts)
                out.append(code)
            done.add(i)
            i = (tail[-1] if tail else i) + 1
            continue
        out.append(lines[i])
        i += 1
    return out, len(found)


def main(argv):
    if len(argv) < 2 or argv[1] not in ('--check', '--fix'):
        print(__doc__)
        return 2
    files = argv[2:] or tracked_sources()
    total = 0
    for path in files:
        with open(path, encoding='utf-8') as f:
            text = f.read()
        lines = text.split('\n')
        if argv[1] == '--check':
            for i, code, comment in offenders(lines):
                print(f'{path}:{i + 1}: trailing comment: {lines[i].strip()}')
                total += 1
        else:
            fixed, moved = fix(lines)
            if moved:
                with open(path, 'w', encoding='utf-8') as f:
                    f.write('\n'.join(fixed))
                total += moved
    if argv[1] == '--check':
        if total:
            print(f'trailing_comments: {total} comment(s) after code — put them on the line above '
                  f'(scripts/trailing_comments.py --fix)', file=sys.stderr)
            return 1
        print(f'trailing_comments: {len(files)} files clean')
        return 0
    print(f'trailing_comments: moved {total} comment(s) in {len(files)} files; run clang-format')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
