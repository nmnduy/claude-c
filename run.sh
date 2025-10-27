export OPENAI_API_KEY=${OPENAI_API_KEY:-$OPENROUTER_API_KEY}
set -xe
make && CLAUDE_THEME=${CLAUDE_THEME:-dracula} \
  OPENAI_API_BASE=${OPENAI_API_BASE:-https://openrouter.ai/api} \
  OPENAI_MODEL=${OPENAI_MODEL:-z-ai/glm-4.6} \
  ./build/claude
