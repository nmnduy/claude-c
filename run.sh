set -xe
make && CLAUDE_C_THEME=${CLAUDE_C_THEME:-kitty-default} \
  ./build/claude-c

# examples config:
# export OPENAI_API_KEY=sk-xxxxxxxxxxxxxxxxxxxxxxxxx \
# OPENAI_API_BASE=${OPENAI_API_BASE:-https://openai.ai/api} \
# OPENAI_MODEL=${OPENAI_MODEL:-o4-mini} \
