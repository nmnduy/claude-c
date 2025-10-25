/*
 * completion.h - Path and File Completion Utilities
 *
 * Provides filesystem-based completion for paths and directories
 */

#ifndef COMPLETION_H
#define COMPLETION_H

#include "lineedit.h"

/**
 * Complete file paths (files and directories)
 *
 * @param partial  Partial path to complete
 * @param ctx      Optional context (unused for now)
 * @return         CompletionResult with matches, or NULL on error
 */
CompletionResult* complete_filepath(const char *partial, void *ctx);

/**
 * Complete directory paths only
 *
 * @param partial  Partial path to complete
 * @param ctx      Optional context (unused for now)
 * @return         CompletionResult with matches, or NULL on error
 */
CompletionResult* complete_dirpath(const char *partial, void *ctx);

#endif // COMPLETION_H
