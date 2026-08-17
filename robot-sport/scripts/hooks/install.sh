#!/usr/bin/env bash
#
# 安装 pre-push 护栏钩子。
#
# 采用「复制到 .git/hooks/」而非 core.hooksPath=scripts/hooks，原因：
# core.hooksPath 指向工作区内的目录时，钩子会随当前检出的分支存在与否而失效——
# 而切到缺少该文件的分支，恰恰是最容易推错的时刻。复制安装则与分支无关，始终生效。
# 代价是脚本更新后需重新运行本脚本。

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
hook_src="$repo_root/scripts/hooks/pre-push"
hook_dst="$(git rev-parse --git-path hooks)/pre-push"

[ -f "$hook_src" ] || { echo "找不到 $hook_src" >&2; exit 1; }

mkdir -p "$(dirname "$hook_dst")"
install -m 755 "$hook_src" "$hook_dst"
echo "已安装：$hook_dst"

# 仅在未配置时写入默认值，不覆盖既有设置
set_default() {
    git config --get "$1" >/dev/null 2>&1 || { git config "$1" "$2"; echo "  设置 $1 = $2"; }
}
set_default hooks.publicRemote 'github\.com'
set_default hooks.publicBranch 'opensource'
set_default hooks.publicTagRef 'refs/opensource-tags'
git config --get-all hooks.internalPath >/dev/null 2>&1 || {
    git config --add hooks.internalPath '.gitea'; echo "  设置 hooks.internalPath = .gitea"
}

if ! git config --get hooks.leakPattern >/dev/null 2>&1; then
    cat <<'EOF'

  尚未配置 hooks.leakPattern —— 内部标识扫描处于关闭状态。
  该正则含内部基础设施信息，因此不入库，需在每台机器本地设置：

      git config hooks.leakPattern '<内网地址|内部域名|token 名|runner 标签|…>'

  多个模式用 | 分隔，ERE 语法。
EOF
else
    echo "  hooks.leakPattern 已配置"
fi
