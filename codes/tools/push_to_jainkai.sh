#!/bin/sh
# tools/push_to_jainkai.sh —— 把本地历史推到 JadeyhCode/JainkAI（走 ssh.github.com:443）
#
# 为什么要这么绕：这台机器 `github.com:443` **被 SNI 阻断**（TCP 通、TLS 超时），
# 而 `ssh.github.com:443` 是通的 ⇒ 用 SSH over 443。前提：`~/.ssh/id_ed25519_jaink.pub`
# 已经被贴到 GitHub（账号级 settings/keys 或仓库级 settings/keys + Allow write access）。
#
# 做法（不覆盖你已有的东西）：先 fetch 他们的 `main`（现有 LICENSE 提交），
# 用 `--allow-unrelated-histories` 并进来，再把本地 `master` 推到 `main`。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
KEY=${KEY:-$HOME/.ssh/id_ed25519_jaink}
URL=ssh://git@ssh.github.com:443/JadeyhCode/JainkAI.git
SSH="ssh -i $KEY -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 -p 443"

echo "== ① 授权测试 =="
if ! $SSH -T git@ssh.github.com 2>&1 | grep -q "successfully authenticated"; then
    $SSH -T git@ssh.github.com 2>&1 | head -2
    echo "  ✗ 还没授权：把下面这行贴到 GitHub（账号级或仓库级部署密钥，仓库级要勾 Allow write access）："
    cat "$KEY.pub" 2>/dev/null || echo "  （本地没有 $KEY.pub，先 ssh-keygen 生成）"
    exit 3
fi
echo "  ✓ 授权过了"

echo "== ② 远端 =="
git remote get-url jaink >/dev/null 2>&1 || git remote add jaink "$URL"
echo "  $(git remote get-url jaink)（不含任何凭据）"

echo "== ③ 取他们的 main 并进来（不覆盖） =="
git fetch jaink main
if git merge-base --is-ancestor jaink/main HEAD 2>/dev/null; then
    echo "  已经包含远端历史，不用并"
else
    git merge --allow-unrelated-histories --no-edit -m "合并 JainkAI 现有 main（保留 LICENSE 提交）" jaink/main
fi

echo "== ④ 推 =="
GIT_SSH_COMMAND="$SSH" git push -u jaink HEAD:main
echo "== ⑤ 核对 =="
GIT_SSH_COMMAND="$SSH" git ls-remote --heads jaink | sed 's/^/  /'
git log --oneline -1 | sed 's/^/  本地最新：/'
echo "推完了：https://github.com/JadeyhCode/JainkAI"
