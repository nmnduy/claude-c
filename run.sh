set -xe
make && CLAUDE_THEME=${CLAUDE_THEME:-dracula} \
  ./build/claude-c
  # OPENAI_API_BASE=${OPENAI_API_BASE:-https://openai.ai/api} \
  # OPENAI_MODEL=${OPENAI_MODEL:-z-ai/glm-4.6} \
