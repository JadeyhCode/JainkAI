#!/bin/sh
# tests/pool.sh — 有界并行调度（POSIX sh / dash 也能跑）
#
# 为什么自己写一份（不用 xargs -P、不用 wait -n）：
#   · 本项目脚本都是 #!/bin/sh，`xargs -P` 不是 POSIX；
#   · dash **没有** `wait -n`，没法"谁先完谁补位"；
#   所以用「任务文件 + mv 原子领取」自己调度 —— 同一文件系统内 mv 是原子的，
#   谁抢到算谁的；抢不到的重试下一条。**只有这一处调度实现**
#   （tests/all.sh 与 tests/probes_build.sh 都 source 它，别各自再抄一份）。
#
# 用法（source 进来，不是当脚本跑）：
#     . tests/pool.sh
#     POOL_DIR=$ROOT/_t/par     # 日志/结果落点（run 会清里面 *.log *.rc 与 todo/run/res）
#     POOL_JOBS=8
#     pool_task_setup()   { ... }   # 可选钩子：在**该任务自己的子壳**里先跑；
#                                   #   $1=任务名 $2=worker 号；在里面 cd 到自己的工作目录
#     pool_task_cleanup() { ... }   # 可选钩子：任务跑完收尾（清沙箱等）
#     pool_run tests/tasks.txt      # 清单每行：<名字>|<命令>
#
# 产物：$POOL_DIR/<名字>.log（该任务全部输出）、$POOL_DIR/<名字>.rc（"<退出码> <毫秒>"）
#      屏幕上每条任务一行：[OK]/[FAIL] <名字> <秒>  <该任务输出的最后一行>
#
# 约定（踩过的坑）：
#   · 任务命令的 stdin 一律接 /dev/null：被测脚本/程序会读 stdin，
#     不接就会把调度器自己的输入吃光（测试脚本里同一个坑已经踩过一次）。
#   · 钩子是在子壳里调用的，所以钩子里的 cd 只影响该任务。

pool_task_setup()   { :; }
pool_task_cleanup() { :; }

pool_worker() {   # $1 = worker 号
    # 血泪：调用方（all.sh）带着 `set -e`，任务失败会把 **worker 自己**弄死，
    # 于是这条任务连 <名字>.rc 都来不及写 —— 汇总时它**静默消失**，
    # 看起来比"失败"还像成功。worker 里必须关掉 errexit，用退出码自己记账。
    set +e
    _pw_k=$1
    while :; do
        _pw_f=$(ls "$POOL_DIR/todo" 2>/dev/null | head -1)
        [ -n "$_pw_f" ] || break
        mv "$POOL_DIR/todo/$_pw_f" "$POOL_DIR/run/$_pw_f" 2>/dev/null || continue
        _pw_name=${_pw_f#*.}
        _pw_cmd=$(cat "$POOL_DIR/run/$_pw_f")
        _pw_s=$(date +%s%N)
        _pw_rc=0
        ( pool_task_setup "$_pw_name" "$_pw_k"; sh -c "$_pw_cmd" ) \
            < /dev/null > "$POOL_DIR/$_pw_name.log" 2>&1 || _pw_rc=$?
        _pw_ms=$(( ($(date +%s%N) - _pw_s) / 1000000 ))
        printf '%s %s\n' "$_pw_rc" "$_pw_ms" > "$POOL_DIR/$_pw_name.rc"
        pool_task_cleanup "$_pw_name" "$_pw_k"
        if [ -n "${POOL_QUIET:-}" ]; then continue; fi
        _pw_last=$(grep -v '^[[:space:]]*$' "$POOL_DIR/$_pw_name.log" 2>/dev/null | tail -1 | cut -c1-64)
        if [ "$_pw_rc" -eq 0 ]; then
            printf '[OK]   %-16s %s.%ss  %s\n' "$_pw_name" \
                "$((_pw_ms / 1000))" "$(( (_pw_ms % 1000) / 100 ))" "$_pw_last"
        else
            printf '[FAIL] %-16s %s.%ss  (exit %s)  %s\n' "$_pw_name" \
                "$((_pw_ms / 1000))" "$(( (_pw_ms % 1000) / 100 ))" "$_pw_rc" "$_pw_last"
        fi
    done
    exit 0
}

pool_run() {   # $1 = 任务清单文件
    _pr_list=$1
    rm -rf "$POOL_DIR/todo" "$POOL_DIR/run" "$POOL_DIR/res"
    rm -f "$POOL_DIR"/*.log "$POOL_DIR"/*.rc
    mkdir -p "$POOL_DIR/todo" "$POOL_DIR/run" "$POOL_DIR/res"
    _pr_n=0
    while IFS= read -r _pr_line; do
        case $_pr_line in ''|'#'*) continue ;; esac
        _pr_n=$((_pr_n + 1))
        printf '%s\n' "${_pr_line#*|}" > "$POOL_DIR/todo/$(printf '%02d' "$_pr_n").${_pr_line%%|*}"
    done < "$_pr_list"
    _pr_k=0
    while [ "$_pr_k" -lt "$POOL_JOBS" ]; do
        _pr_k=$((_pr_k + 1))
        pool_worker "$_pr_k" &
    done
    wait || true
}
