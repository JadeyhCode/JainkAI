#!/bin/sh
# tools/push_to_github.sh —— 把当前分支推到 GitHub（**不在任何地方留 token**）
#
# 为什么要有脚本：这台机器的 `/etc/hosts` 被 Steam++ 写进了 github 系域名的静态指向
# （`127.0.0.1 github.com` 之类），加速没开时**所有** github 流量都会打到本地空端口
# （curl 报 HTTP 000）。脚本第一步就体检这件事，省得对着 "Could not resolve host" 瞎猜。
#
# 用法：
#   GH_TOKEN=<你的 token> sh tools/push_to_github.sh <owner>/<repo> [分支]
#   （token 只从环境变量读，进不了 argv、进不了 git config、进不了任何文件）
#
# 说明：
#   · 远端用**不带 token 的** URL 记住（`git@`/`https://github.com/owner/repo.git`），
#     推的时候才把 token 拼进这次命令 —— 之后 `git remote -v` 里看不到任何凭据。
#   · 已存在同名远端就直接推；不存在就加一个 `origin`。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
REPO=${1:-}
BRANCH=${2:-$(git rev-parse --abbrev-ref HEAD)}
[ -n "$REPO" ] || { echo "用法：GH_TOKEN=<token> sh $0 <owner>/<repo> [分支]"; exit 2; }
[ -n "${GH_TOKEN:-}" ] || { echo "没有 GH_TOKEN（从环境变量读，别写进命令行历史）"; exit 2; }

echo "== ① 体检：github 通不通 =="
IP=$(getent hosts github.com 2>/dev/null | awk '{print $1; exit}')
echo "  github.com → ${IP:-（解析不出来）}"
case "${IP:-}" in
    127.0.0.1|::1|"")
        echo "  ✗ 指向本机/解析不了 ⇒ 这是 /etc/hosts 里 Steam++ 写的静态指向，且加速没开。"
        echo "    两个办法（选一个，都要你动手）："
        echo "      a) 打开 Steam++ → 网络加速 → 启动（它会在本地起代理，那些 hosts 才有用）；"
        echo "      b) 把那些指向删掉： sudo sed -i '/github/d' /etc/hosts"
        exit 3 ;;
esac
code=$(curl -sS -m 8 -o /dev/null -w '%{http_code}' -H "User-Agent: aipoject-push" \
       -H "Authorization: token $GH_TOKEN" https://api.github.com/user 2>/dev/null || echo 000)
echo "  api.github.com → HTTP $code"
[ "$code" = "200" ] || { echo "  ✗ token 无效或网络不通（HTTP $code）"; exit 4; }

echo "== ② 远端 =="
if git remote get-url origin >/dev/null 2>&1; then
    echo "  已有 origin：$(git remote get-url origin)"
else
    git remote add origin "https://github.com/$REPO.git"
    echo "  新加 origin：https://github.com/$REPO.git（**不含 token**）"
fi

echo "== ③ 推 $BRANCH =="
# token 只在这一次命令里出现；`http.extraheader` 不落盘（-c 是临时配置）
git -c http.extraheader="Authorization: Basic $(printf 'x-access-token:%s' "$GH_TOKEN" | base64 | tr -d '\n')" \
    push -u origin "$BRANCH"
echo "== ④ 核对 =="
git remote -v | sed 's/^/  /'
echo "  （远端 URL 里不该有 token —— 上面看不到就是对的）"
echo "推完了。记得到 GitHub 上把这个 token 撤掉重发（它进过聊天记录）。"
