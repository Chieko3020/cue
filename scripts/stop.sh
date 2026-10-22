#!/usr/bin/env bash
# 一条命令停掉 Cue 的全部进程（需求文档 FR-OBS.4：`./run.sh` 起全套，`./stop.sh` 全停）。
#
# 为什么单独写一个而不是靠 Ctrl-C：`run.sh demo-min` 在同一个终端里前台跑，Ctrl-C 能连带
# 它自己的 trap 清理；但 `serve.sh` 是 `exec` 到 cue-serve，`cue-eval` 可能是后台跑的，
# 还有手工起的 `cued`/`cue-probe`/`cue-capture`——这些的父子关系各不相同，只有一个按
# 进程名收敛的入口才停得干净。
#
# 用法：
#   ./scripts/stop.sh              # 优雅停（SIGTERM，宽限 5 s，再 SIGKILL）
#   ./scripts/stop.sh --timeout 10 # 改宽限
#   ./scripts/stop.sh --force      # 直接 SIGKILL
#   ./scripts/stop.sh --dry-run    # 只列出会停哪些，不动
#   ./scripts/stop.sh --keep-probe # 保留 cue-probe（跨机时它常常不在本机，或不该被本地 stop 波及）
#
# 退出码：0 全部已停 / 1 有进程在宽限后仍存活（已尝试 SIGKILL）/ 2 用法错误
set -uo pipefail

TIMEOUT=5
FORCE=0
DRY=0
KEEP_PROBE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --timeout) TIMEOUT="${2:-5}"; shift 2 || { echo "用法：--timeout N" >&2; exit 2; } ;;
    --force) FORCE=1; shift ;;
    --dry-run) DRY=1; shift ;;
    --keep-probe) KEEP_PROBE=1; shift ;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数：$1（--help 看用法）" >&2; exit 2 ;;
  esac
done

# 用**精确进程名**而不是 `pkill -f <路径>`：`-f` 匹配整条命令行，
# 会把"恰好命令行里含有这个路径"的 shell 自己也杀掉（这个坑实际踩过两次）。
# 顺序有讲究：先停采集端（不再产生新帧），再停管线，最后停探针与控制台。
NAMES=(cue-capture cued cue cue-serve cue-probe)
[ "$KEEP_PROBE" = "1" ] && NAMES=(cue-capture cued cue cue-serve)

found_any=0
for n in "${NAMES[@]}"; do
  pids=$(pgrep -x "$n" 2>/dev/null | tr '\n' ' ')
  [ -z "$pids" ] && continue
  found_any=1
  echo "[stop] $n: $pids"
done

if [ "$found_any" = "0" ]; then
  echo "[stop] 没有在跑的 Cue 进程"
  exit 0
fi

if [ "$DRY" = "1" ]; then
  echo "[stop] --dry-run：以上进程会被停掉（实际未动）"
  exit 0
fi

# SIGTERM：进程自己有机会写收尾事件、关 socket、清理 /tmp/cue/<pid>/
for n in "${NAMES[@]}"; do
  pids=$(pgrep -x "$n" 2>/dev/null)
  [ -z "$pids" ] && continue
  if [ "$FORCE" = "1" ]; then
    kill -KILL $pids 2>/dev/null
  else
    kill -TERM $pids 2>/dev/null
  fi
done

[ "$FORCE" = "1" ] && { TIMEOUT=0; }

# 宽限等待：每 100 ms 看一次，全没了就提前退出（不白等满）
deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
  alive=0
  for n in "${NAMES[@]}"; do
    pgrep -x "$n" >/dev/null 2>&1 && alive=$((alive + 1))
  done
  [ "$alive" = "0" ] && break
  sleep 0.1
done

# 还有活着的：SIGKILL 兜底，并如实报告（不要假装停干净了）
still=""
for n in "${NAMES[@]}"; do
  pids=$(pgrep -x "$n" 2>/dev/null | tr '\n' ' ')
  [ -n "$pids" ] && still="$still$n($pids) "
done

if [ -n "$still" ]; then
  echo "[stop] 宽限 ${TIMEOUT}s 后仍存活，SIGKILL：$still" >&2
  for n in "${NAMES[@]}"; do
    pids=$(pgrep -x "$n" 2>/dev/null)
    [ -n "$pids" ] && kill -KILL $pids 2>/dev/null
  done
  sleep 0.5
  left=""
  for n in "${NAMES[@]}"; do
    pgrep -x "$n" >/dev/null 2>&1 && left="$left$n "
  done
  if [ -n "$left" ]; then
    echo "[stop] SIGKILL 后仍在：$left（可能卡在不可中断的系统调用里，需要人工排查）" >&2
    exit 1
  fi
fi

# 控制 socket 的清理：正常退出时进程自己会清 /tmp/cue/<pid>/；
# 被 SIGKILL 的会留下目录（0700、按 pid 命名，不影响下次启动，但会一直攒）。
# 这里只删**确认没有活进程在用的**那些：目录名就是 pid。
removed=0
for d in /tmp/cue/*/; do
  [ -d "$d" ] || continue
  pid=$(basename "$d")
  case "$pid" in ''|*[!0-9]*) continue ;; esac    # 非数字目录不动
  kill -0 "$pid" 2>/dev/null && continue          # 还有活进程在用它
  rm -rf "$d" 2>/dev/null && removed=$((removed + 1))
done
[ "$removed" -gt 0 ] && echo "[stop] 清理了 $removed 个遗留的控制 socket 目录（/tmp/cue/）"

echo "[stop] 已全部停止"
