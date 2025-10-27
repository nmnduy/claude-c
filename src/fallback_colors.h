/*
 * fallback_colors.h - Centralized Fallback Color Definitions
 * 
 * This header provides the single source of truth for all fallback ANSI colors
 * used throughout the application when the colorscheme system is not available
 * or fails to load. All modules should use these constants instead of defining
 * their own ANSI color codes.
 */

#ifndef FALLBACK_COLORS_H
#define FALLBACK_COLORS_H

// ANSI reset code - used to reset all formatting
#define ANSI_RESET "\033[0m"

// Primary color mappings for different UI elements
// These are used when colorscheme colors are not available

// User messages - Green (standard terminal color)
#define ANSI_FALLBACK_USER "\033[32m"

// Assistant messages - Blue (standard terminal color)  
#define ANSI_FALLBACK_ASSISTANT "\033[34m"

// Tool execution - Yellow (standard terminal color)
#define ANSI_FALLBACK_TOOL "\033[33m"

// Error messages - Red (standard terminal color)
#define ANSI_FALLBACK_ERROR "\033[31m"

// Status messages - Cyan (standard terminal color)
#define ANSI_FALLBACK_STATUS "\033[36m"

// Headers/accents - Bold Cyan (for emphasis)
#define ANSI_FALLBACK_HEADER "\033[1;36m"

// Additional colors for specific use cases
#define ANSI_FALLBACK_GREEN "\033[32m"
#define ANSI_FALLBACK_YELLOW "\033[33m"
#define ANSI_FALLBACK_BLUE "\033[34m"
#define ANSI_FALLBACK_MAGENTA "\033[35m"
#define ANSI_FALLBACK_CYAN "\033[36m"
#define ANSI_FALLBACK_WHITE "\033[37m"

// Bold variants for emphasis
#define ANSI_FALLBACK_BOLD_GREEN "\033[1;32m"
#define ANSI_FALLBACK_BOLD_YELLOW "\033[1;33m"
#define ANSI_FALLBACK_BOLD_BLUE "\033[1;34m"
#define ANSI_FALLBACK_BOLD_RED "\033[1;31m"
#define ANSI_FALLBACK_BOLD_MAGENTA "\033[1;35m"
#define ANSI_FALLBACK_BOLD_CYAN "\033[1;36m"
#define ANSI_FALLBACK_BOLD_WHITE "\033[1;37m"

// Text formatting codes
#define ANSI_FALLBACK_BOLD "\033[1m"
#define ANSI_FALLBACK_DIM "\033[2m"

// Helper function to get fallback color by element type
// This provides a programmatic way to access fallback colors
// Returns the ANSI escape sequence for given element
static inline const char* get_fallback_color(int element_type) {
    switch (element_type) {
        case 0: // USER
            return ANSI_FALLBACK_USER;
        case 1: // ASSISTANT  
            return ANSI_FALLBACK_ASSISTANT;
        case 2: // TOOL
            return ANSI_FALLBACK_TOOL;
        case 3: // ERROR
            return ANSI_FALLBACK_ERROR;
        case 4: // STATUS
            return ANSI_FALLBACK_STATUS;
        case 5: // HEADER
            return ANSI_FALLBACK_HEADER;
        default:
            return "";
    }
}

// Element type constants for use with get_fallback_color()
#define FALLBACK_USER 0
#define FALLBACK_ASSISTANT 1
#define FALLBACK_TOOL 2
#define FALLBACK_ERROR 3
#define FALLBACK_STATUS 4
#define FALLBACK_HEADER 5

#endif // FALLBACK_COLORS_H