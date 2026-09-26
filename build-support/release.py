#!/usr/bin/env python3
"""给 ZDock 打 GitHub Release 并上传交付产物。

用法：
    <python> build-support/release.py --tag v0.1.4 \
        --exe _review/ZDock_0.1.4.exe --config _review/config.json \
        --notes build/_review/release_notes_v0.1.4.md

凭据：从 git credential helper（GCM）取，**只在内存里**，不落盘、不打印。
      取不到就报错退出（别在这里交互式输入密码）。

⚠ 两个容易踩的点：
  1. **资产上传端点是 `uploads.github.com`**，不是 `api.github.com`
     （用后者会 404，报错信息完全看不出原因）。
  2. 未认证 API 限流是 60 次/小时，所以每个请求都带 token。

幂等：release 已存在时不重建，只补传缺失的资产。
"""

import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

REPO = 'James-ctrl-Doyle/ZDock'
API = 'https://api.github.com'
UPLOADS = 'https://uploads.github.com'


def get_token():
    """从 git credential helper 取 GitHub token（不落盘）。"""
    try:
        p = subprocess.run(
            ['git', 'credential', 'fill'],
            input='protocol=https\nhost=github.com\n\n',
            capture_output=True, text=True, timeout=30)
    except Exception as e:                       # noqa: BLE001
        print('!! 取凭据失败：%s' % e)
        return None
    for line in p.stdout.splitlines():
        if line.startswith('password='):
            return line[len('password='):].strip()
    print('!! git credential fill 没返回 password（GCM 可能未登录）')
    print('   提示：先随便 git push 一次让它缓存凭据')
    return None


def req(method, url, token, data=None, ctype='application/json'):
    r = urllib.request.Request(url, data=data, method=method)
    r.add_header('Authorization', 'token %s' % token)
    r.add_header('Accept', 'application/vnd.github+json')
    r.add_header('User-Agent', 'zdock-release')
    if data is not None:
        r.add_header('Content-Type', ctype)
    try:
        with urllib.request.urlopen(r, timeout=120) as resp:
            body = resp.read()
            return resp.status, (json.loads(body) if body else None)
    except urllib.error.HTTPError as e:
        body = e.read().decode('utf-8', 'replace')
        return e.code, body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tag', required=True)
    ap.add_argument('--exe', required=True, help='要上传的 exe（会作为 release 资产）')
    ap.add_argument('--config', default=None, help='可选：一起上传的 config.json')
    ap.add_argument('--notes', default=None, help='release notes 的 markdown 文件')
    ap.add_argument('--target', default=None, help='tag 指向的 commit（默认当前 HEAD）')
    ap.add_argument('--name', default=None, help='release 标题（默认 tag）')
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        print('!! 找不到 %s' % args.exe)
        return 1

    token = get_token()
    if not token:
        return 1

    target = args.target
    if not target:
        target = subprocess.run(['git', 'rev-parse', 'HEAD'],
                                capture_output=True, text=True).stdout.strip()
    print('repo   : %s' % REPO)
    print('tag    : %s' % args.tag)
    print('target : %s' % target)
    print('exe    : %s (%d bytes)' % (args.exe, os.path.getsize(args.exe)))

    notes = ''
    if args.notes and os.path.exists(args.notes):
        with open(args.notes, encoding='utf-8') as f:
            notes = f.read()

    # ---- 已存在？ ----
    st, body = req('GET', '%s/repos/%s/releases/tags/%s' % (API, REPO, args.tag), token)
    if st == 200:
        rel = body
        print('release 已存在：id=%s' % rel['id'])
    elif st == 404:
        payload = json.dumps({
            'tag_name': args.tag,
            'target_commitish': target,
            'name': args.name or args.tag,
            'body': notes,
            'draft': False,
            'prerelease': False,
        }).encode('utf-8')
        st, rel = req('POST', '%s/repos/%s/releases' % (API, REPO), token, payload)
        if st not in (200, 201):
            print('!! 创建 release 失败 HTTP %s\n%s' % (st, rel))
            return 1
        print('release 已创建：id=%s  %s' % (rel['id'], rel['html_url']))
    else:
        print('!! 查询 release 失败 HTTP %s\n%s' % (st, body))
        return 1

    # ---- 上传资产（已同名则跳过）----
    st, assets = req('GET', '%s/repos/%s/releases/%s/assets' % (API, REPO, rel['id']), token)
    have = {a['name'] for a in assets} if st == 200 and isinstance(assets, list) else set()

    uploads = [args.exe] + ([args.config] if args.config else [])
    for path in uploads:
        name = os.path.basename(path)
        if name in have:
            print('资产已存在，跳过：%s' % name)
            continue
        with open(path, 'rb') as f:
            blob = f.read()
        url = '%s/repos/%s/releases/%s/assets?name=%s' % (UPLOADS, REPO, rel['id'], name)
        st, res = req('POST', url, token, blob, ctype='application/octet-stream')
        if st in (200, 201):
            print('已上传：%s (%d bytes)  %s' % (name, len(blob), res.get('browser_download_url', '')))
        else:
            print('!! 上传 %s 失败 HTTP %s\n%s' % (name, st, res))
            return 1

    print('\n完成：%s' % rel['html_url'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
