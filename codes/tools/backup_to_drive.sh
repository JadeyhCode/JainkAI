#!/bin/sh
# tools/backup_to_drive.sh —— 把整个项目**一键备份到移动硬盘**（含 .git），并留下可核对的口子
#
# 为什么要有脚本：手动 cp 容易漏（`.git`、`docs/`、`codeprint`…），而且"备份完了吗"没法核对。
# 这个脚本会：① 找到那块**外接盘**（不是根分区）② 挂上（能 udisksctl 就不用 sudo）
# ③ 用 rsync 全量镜像到一个**带日期**的目录 ④ 打印文件数 + 内容清单的 md5（可核对）。
#
# 用法：sh tools/backup_to_drive.sh [目标目录名]
#   目标盘上的目录默认：aipoject-backup-YYYYMMDD
#   想换盘：DRIVE=/dev/sdb1 sh tools/backup_to_drive.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAMP=$(date +%Y%m%d)
DESTNAME=${1:-aipoject-backup-$STAMP}

say() { printf '%s\n' "$*"; }

# ---- ① 找外接盘：不是根所在设备的块设备分区，且**有文件系统** ----
ROOTDEV=$(findmnt -no SOURCE / 2>/dev/null | sed 's/[0-9]*$//' || echo "")
DEV=${DRIVE:-}
if [ -z "$DEV" ]; then
    # ⚠ `lsblk -n` 只去表头，**不去树线**（`├─/dev/sda1`）—— 必须加 `-l`（列表模式），
    #   否则设备名带着 `├─`，后面的 mount/lsblk 全都不认（第一版就是这么错的）。
    DEV=$(lsblk -lpno NAME,TYPE,FSTYPE 2>/dev/null | awk -v root="$ROOTDEV" '
        $2=="part" && $3!="" { dev=$1; sub(/[0-9]+$/,"",dev); if (dev!=root) print $1 }' | head -1)
fi
if [ -z "$DEV" ]; then
    say "没找到外接盘（根分区在 $ROOTDEV）。"
    say "请先把那块移动硬盘插上（USB），再重跑这个脚本；或指定设备：DRIVE=/dev/sdb1 sh $0"
    say "插上后可以先看一眼：lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINT"
    exit 2
fi

# ---- ② 挂载：能用 udisksctl（免 sudo）就用它；已经挂着的直接用 ----
MP=$(lsblk -lno MOUNTPOINT "$DEV" 2>/dev/null | head -1)
if [ -z "$MP" ]; then
    if command -v udisksctl >/dev/null 2>&1; then
        say "挂载 $DEV（udisksctl，不需要 sudo）…"
        udisksctl mount -b "$DEV" >/dev/null 2>&1 || true
        MP=$(lsblk -lno MOUNTPOINT "$DEV" 2>/dev/null | head -1)
    fi
fi
if [ -z "$MP" ]; then
    say "$DEV 没挂上。请手工挂一次（然后重跑本脚本）："
    say "  sudo mkdir -p /mnt/wd500 && sudo mount $DEV /mnt/wd500"
    say "或（桌面环境，免 sudo）：udisksctl mount -b $DEV"
    exit 3
fi

TIP=$(df -h "$MP" | awk 'NR==2{print $4" 可用（总 "$2"，已用 "$5"）"}')
say "外接盘：$DEV → $MP   $TIP"
DEST="$MP/$DESTNAME"

# ---- ③ rsync 全量镜像（`_t/` 是临时件，不备份；其余一个不漏，含 .git）----
if command -v rsync >/dev/null 2>&1; then
    say "同步到 $DEST …"
    mkdir -p "$DEST"
    rsync -a --delete --info=stats2 \
        --exclude '_t/' --exclude 'build_*' --exclude '*.o' \
        "$ROOT"/ "$DEST"/ | tail -6
else
    say "没有 rsync，用 cp -a 全量拷（会慢一些）…"
    mkdir -p "$DEST"
    (cd "$ROOT" && tar --exclude='_t' --exclude='build_*' -cf - .) | (cd "$DEST" && tar -xf -)
fi

# ---- ④ 核对：文件数 + 内容清单的 md5（两边一致才算备份成功）----
man() { (cd "$1" && find . -type f -not -path './_t/*' -print0 | sort -z | xargs -0 md5sum 2>/dev/null | md5sum | cut -d' ' -f1); }
NF=$(cd "$DEST" && find . -type f | wc -l)
SZ=$(du -sh "$DEST" | cut -f1)
H1=$(man "$ROOT"); H2=$(man "$DEST")
say ""
say "备份完成：$DEST"
say "  文件数 $NF    体积 $SZ"
say "  源清单 md5   $H1"
say "  备份清单 md5 $H2"
if [ "$H1" = "$H2" ]; then
    say "  → **两边一致**（备份可核对；`_t/` 临时件按设计没备）"
else
    say "  → ⚠ 两边**不一致**：别删源！重跑一次，或看 rsync 报错。"
    exit 4
fi
say ""
say "以后恢复：把 $DEST 拷回任意目录即可（含 .git，`git log` 就在）。"
