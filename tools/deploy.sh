#!/usr/bin/env bash
set -e

REPO="https://github.com/maxwofford/dandelion.git"
DIR="$HOME/gloam"
SESSION="gloam"

# clone or pull
if [ -d "$DIR/.git" ]; then
  echo "updating $DIR..."
  git -C "$DIR" pull --ff-only
else
  echo "cloning into $DIR..."
  git clone "$REPO" "$DIR"
fi

# check deps
command -v bun >/dev/null || { echo "bun not found — install: curl -fsSL https://bun.sh/install | bash"; exit 1; }
command -v git >/dev/null || { echo "git not found"; exit 1; }

# kill existing session if running
tmux kill-session -t "$SESSION" 2>/dev/null || true

# start tmux session with restart loop
tmux new-session -d -s "$SESSION" "cd $DIR && while true; do echo '--- starting gloam server ---'; bun server/index.js; echo '--- server exited, restarting in 2s ---'; sleep 2; done"

echo "gloam running in tmux session '$SESSION'"
echo "  attach:  tmux attach -t $SESSION"
echo "  update:  cd $DIR && git pull  (server restarts on next crash or ctrl-c)"
echo "  restart: tmux send-keys -t $SESSION C-c"
