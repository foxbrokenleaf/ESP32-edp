"""Check the embedded web page in main/http.c without flashing the device.

What it does:
  1. rebuilds INDEX_HTML[] (a C array of string literals) into the real page text;
  2. extracts the <script> block and runs `node --check` on it (skipped when node
     is not installed);
  3. checks that every element id used by the script exists in the HTML, or is
     created by the script itself (the checklist rows build listText0..7 etc.);
  4. checks that every /api/xxx endpoint used by the page is registered in routes[].

Usage (from the project root):

    python tools/check_web_page.py
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTTP_C = os.path.join(ROOT, 'main', 'http.c')

ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', "'": "'", '\\': '\\', '0': '\0'}
ARRAY_START = 'static const char *const INDEX_HTML[] = {'


def unescape_literal(literal):
    out = []
    i = 0
    while i < len(literal):
        ch = literal[i]
        if ch == '\\' and i + 1 < len(literal):
            out.append(ESCAPES.get(literal[i + 1], literal[i + 1]))
            i += 2
        else:
            out.append(ch)
            i += 1
    return ''.join(out)


def extract_html(source):
    start = source.index(ARRAY_START)
    end = source.index('\n};', start)
    lines = []
    for raw in source[start:end].splitlines()[1:]:
        text = raw.strip()
        if not text:
            continue
        if not text.startswith('"'):
            raise SystemExit('unexpected line in INDEX_HTML: %r' % raw)
        lines.append(unescape_literal(text[text.index('"') + 1:text.rindex('"')]))
    return '\n'.join(lines), source[end:]


def check_js(page):
    match = re.search(r'<script>(.*?)</script>', page, re.S)
    if match is None:
        raise SystemExit('no <script> block found in the page')
    js = match.group(1)

    handle = tempfile.NamedTemporaryFile('w', suffix='.js', delete=False, encoding='utf-8')
    try:
        handle.write(js)
        handle.close()
        result = subprocess.run(['node', '--check', handle.name],
                                capture_output=True, text=True)
    except FileNotFoundError:
        return js, 'skipped (node not installed)'
    finally:
        if os.path.exists(handle.name):
            os.remove(handle.name)

    if result.returncode != 0:
        raise SystemExit('node --check failed:\n' + result.stderr)
    return js, 'ok'


def check_ids(page, js):
    html_ids = set(re.findall(r"id='([A-Za-z0-9_-]+)'", page))
    used = set(re.findall(r"el\('([A-Za-z0-9_-]+)'\)", js))

    limit = re.search(r'const LIST_MAX = (\d+);', js)
    dynamic = set()
    if limit is not None:
        for prefix in re.findall(r"el\('([A-Za-z0-9_-]+)' \+ i\)", js):
            dynamic |= {prefix + str(i) for i in range(int(limit.group(1)))}

    missing = sorted(used - html_ids - dynamic)
    unused = sorted(html_ids - used - dynamic - {'pageAbout', 'pageCourse',
                                                 'pageChecklist', 'listRows'})
    return missing, unused


def check_routes(page, tail):
    apis = sorted(set(re.findall(r"fetch\('(/api/[a-z]+)'", page)))
    routes = set(re.findall(r'\.uri = "(/api/[a-z]+)"', tail))
    return apis, sorted(set(apis) - routes)


def main():
    with open(HTTP_C, encoding='utf-8') as fp:
        source = fp.read()

    page, tail = extract_html(source)
    print('page          : %d lines, %d bytes' % (page.count('\n') + 1, len(page.encode('utf-8'))))

    js, js_status = check_js(page)
    print('node --check  : %s' % js_status)

    missing, unused = check_ids(page, js)
    print('ids in html   : ok' if not missing else 'ids missing in html: %s' % missing)
    if unused:
        print('ids unused    : %s' % unused)

    apis, unregistered = check_routes(page, tail)
    print('api used      : %s' % ', '.join(apis))
    if unregistered:
        print('api NOT routed: %s' % unregistered)

    ok = not missing and not unregistered
    print('result        : %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
