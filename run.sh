set -xe
make &&  CLAUDE_THEME=./colorschemes/kitty-default.conf \
  OPENAI_API_KEY=$OPENROUTER_API_KEY \
  OPENAI_API_BASE=https://openrouter.ai/api \
  OPENAI_MODEL=z-ai/glm-4.6 \
  ./build/claude
