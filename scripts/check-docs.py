#!/usr/bin/env python3
"""문서 검사. make test 의 T-00 이 실행한다.

1. 문서의 상대 링크와 앵커가 살아 있는가
2. 요구사항 번호(F-, C-, N-)가 모두 테스트 명세에 대응되는가
3. 테스트 명세의 번호(T-)와 scripts/verify.sh 의 번호가 서로 같은가
4. 성능 예측을 쓰기 전에 서버·클라이언트 구현이 들어오지 않았는가
"""
import glob
import os
import re
import sys
import urllib.parse

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
errors = []


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def strip_code(text):
    return re.sub(r'```.*?```', '', text, flags=re.S)


def slug(heading):
    h = re.sub(r'`', '', heading.strip().lower())
    h = re.sub(r'[^\w\- ]', '', h)
    return h.replace(' ', '-')


# 1. 링크와 앵커
files = sorted({os.path.normpath(f) for f in glob.glob('**/*.md', recursive=True)} |
               {os.path.normpath(f) for f in glob.glob('.github/**/*.md', recursive=True)})
anchors = {}
for f in files:
    anchors[f] = {slug(m.group(1)) for m in re.finditer(r'^#{1,6}\s+(.+?)\s*$', strip_code(read(f)), flags=re.M)}
n_links = 0
for f in files:
    for m in re.finditer(r'\[[^\]]*\]\(<?([^)>]+)>?\)', strip_code(read(f))):
        url = m.group(1).strip()
        if url.startswith(('http://', 'https://', 'mailto:')):
            continue
        n_links += 1
        path, _, anchor = url.partition('#')
        path = urllib.parse.unquote(path)
        target = os.path.normpath(os.path.join(os.path.dirname(f), path)) if path else f
        if not os.path.exists(target):
            errors.append(f'{f}: 없는 파일을 가리킨다 -> {url}')
        elif anchor and target.endswith('.md') and urllib.parse.unquote(anchor) not in anchors.get(target, set()):
            errors.append(f'{f}: 없는 제목을 가리킨다 -> {url}')

# 2. 요구사항 -> 테스트 명세
REQ = 'docs/func-specs/README.md'
TEST = 'docs/test-specs/README.md'
req_ids = re.findall(r'^\| ([FCN]-\d+) \|', read(REQ), flags=re.M)
test_text = read(TEST)
mentioned = set(re.findall(r'\b([FCN]-\d+)\b', test_text))
for rid in req_ids:
    if rid not in mentioned:
        errors.append(f'{REQ}: {rid} 가 테스트 명세 어디에도 대응되지 않는다')
for rid in sorted(mentioned - set(req_ids)):
    errors.append(f'{TEST}: 요구사항에 없는 번호 {rid} 를 쓴다')

# 3. 테스트 명세 <-> verify.sh
spec_tests = set(re.findall(r'^\| (T-\d+) \|', test_text, flags=re.M))
script_tests = set(re.findall(r'\bT-\d+\b', read('scripts/verify.sh')))
for tid in sorted(spec_tests - script_tests):
    errors.append(f'scripts/verify.sh: 테스트 명세의 {tid} 가 없다')
for tid in sorted(script_tests - spec_tests):
    errors.append(f'{TEST}: verify.sh 의 {tid} 가 명세에 없다')

# 4. 예측 선행
PRED = 'docs/records/prediction.md'
if '상태: 미작성' in read(PRED):
    for src in ('src/common/server.c', 'src/common/client.c'):
        if os.path.exists(src):
            errors.append(f'{src}: 성능 예측({PRED})을 커밋하기 전에는 만들 수 없다')

for e in errors:
    print('문제:', e)
print(f'문서 {len(files)}개, 링크 {n_links}개, 요구사항 {len(req_ids)}개, 테스트 {len(spec_tests)}개 검사, 문제 {len(errors)}개')
sys.exit(1 if errors else 0)
