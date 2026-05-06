/**
 * Compatibility layer for krep tests
 * Provides wrappers around search functions for tests that
 * don't need line counting or position tracking features.
 */

#ifndef TEST_COMPAT_H
#define TEST_COMPAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>          // For SIZE_MAX
#include <regex.h>           // For regex_t
#include "../krep.h"         // Include main header for the full function declarations
#include "../aho_corasick.h" // Add this include

#ifdef TESTING
/*
 * Wrapper functions calling the full search implementations with default
 * arguments for line counting and position tracking parameters.
 * These wrappers effectively simulate the older function signatures.
 */

/* Boyer-Moore-Horspool compatibility wrapper */
static inline uint64_t boyer_moore_search_compat(
    const char *text, size_t text_len,
    const char *pattern, size_t pattern_len,
    bool case_sensitive, size_t report_limit_offset)
{
    // Apply report_limit_offset by limiting the text length if needed
    size_t effective_len = (report_limit_offset < text_len) ? report_limit_offset : text_len;

    // Create a search_params_t structure with the required parameters
    search_params_t params = {
        .pattern = pattern,
        .pattern_len = pattern_len,
        .case_sensitive = case_sensitive,
        .use_regex = false,
        .track_positions = false,
        .count_lines_mode = false,
        .compiled_regex = NULL};

    // Call the new function signature with the adjusted text length
    return boyer_moore_search(&params, text, effective_len, NULL);
}

/* Knuth-Morris-Pratt compatibility wrapper */
static inline uint64_t kmp_search_compat(
    const char *text, size_t text_len,
    const char *pattern, size_t pattern_len,
    bool case_sensitive, size_t report_limit_offset)
{
    // Apply report_limit_offset by limiting the text length if needed
    size_t effective_len = (report_limit_offset < text_len) ? report_limit_offset : text_len;

    // Create a search_params_t structure with the required parameters
    search_params_t params = {
        .pattern = pattern,
        .pattern_len = pattern_len,
        .case_sensitive = case_sensitive,
        .use_regex = false,
        .track_positions = false,
        .count_lines_mode = false,
        .compiled_regex = NULL};

    // Call the new function signature with the adjusted text length
    return kmp_search(&params, text, effective_len, NULL);
}

/* Regex compatibility wrapper */
static inline uint64_t regex_search_compat(
    const char *text, size_t text_len,
    const regex_t *compiled_regex,
    size_t report_limit_offset KREP_UNUSED) // Mark as unused, test refactored
{
    size_t effective_len = text_len; // Use full length

    // Create a search_params_t structure with the required parameters
    search_params_t params = {
        .pattern = NULL, // Not used for regex
        .pattern_len = 0,
        .case_sensitive = false, // Note: Case sensitivity is part of compiled_regex flags
        .use_regex = true,
        .track_positions = false,   // Compatibility wrapper doesn't track positions
        .count_lines_mode = false,  // Compatibility wrapper counts matches by default
        .count_matches_mode = true, // Explicitly set for clarity
        .compiled_regex = compiled_regex,
        .max_count = SIZE_MAX // Default: no limit for compat wrapper
    };
    // Assign multi-pattern fields (even for single regex)
    params.patterns = NULL; // No specific pattern string needed here
    params.pattern_lens = NULL;
    params.num_patterns = 1; // Assume one regex pattern

    // Call the new function signature with the adjusted text length
    return regex_search(&params, text, effective_len, NULL);
}

/* SIMD SSE2 compatibility wrapper */
#ifdef __SSE2__
static inline uint64_t simd_sse2_search_compat(
    const char *text, size_t text_len,
    const char *pattern, size_t pattern_len,
    bool case_sensitive, size_t report_limit_offset)
{
    // Apply report_limit_offset by limiting the text length if needed
    size_t effective_len = (report_limit_offset < text_len) ? report_limit_offset : text_len;

    // Create a search_params_t structure with the required parameters
    search_params_t params = {
        .pattern = pattern,
        .pattern_len = pattern_len,
        .case_sensitive = case_sensitive,
        .use_regex = false,
        .track_positions = false,
        .count_lines_mode = false,
        .compiled_regex = NULL};

    // Call the new function signature with the adjusted text length
    return simd_sse2_search(&params, text, effective_len, NULL);
}
#endif

/* AVX2 compatibility wrapper */
#ifdef __AVX2__
static inline uint64_t simd_avx2_search_compat(
    const char *text, size_t text_len,
    const char *pattern, size_t pattern_len,
    bool case_sensitive, size_t report_limit_offset)
{
    // Apply report_limit_offset by limiting the text length if needed
    size_t effective_len = (report_limit_offset < text_len) ? report_limit_offset : text_len;

    // Create a search_params_t structure with the required parameters
    search_params_t params = {
        .pattern = pattern,
        .pattern_len = pattern_len,
        .case_sensitive = case_sensitive,
        .use_regex = false,
        .track_positions = false,
        .count_lines_mode = false,
        .compiled_regex = NULL};

    // Call the new function signature with the adjusted text length
    return simd_avx2_search(&params, text, effective_len, NULL);
}
#endif

/* NEON compatibility wrapper */
#ifdef __ARM_NEON
static inline uint64_t neon_search_compat(
    const char *text, size_t text_len,
    const char *pattern, size_t pattern_len,
    bool case_sensitive, size_t report_limit_offset)
{
    // Apply report_limit_offset by limiting the text length if needed
    size_t effective_len = (report_limit_offset < text_len) ? report_limit_offset : text_len;

    // Create a search_params_t structure with the required parameters
    search_params_t params = {
        .pattern = pattern,
        .pattern_len = pattern_len,
        .case_sensitive = case_sensitive,
        .use_regex = false,
        .track_positions = false,
        .count_lines_mode = false,
        .compiled_regex = NULL};

    // Call the new function signature with the adjusted text length
    return neon_search(&params, text, effective_len, NULL);
}
#endif

/* Aho-Corasick compatibility wrapper for multiple pattern search */
static inline uint64_t aho_corasick_search_compat(
    const char *text, size_t text_len,
    const char **patterns, size_t *pattern_lens, size_t num_patterns,
    bool case_sensitive, size_t report_limit_offset)
{
    size_t effective_len = text_len; // Use full length
    (void)report_limit_offset;       // Mark as unused

    // Create a search_params_t structure for multiple patterns
    search_params_t params = {
        .pattern = NULL,  // Not used for multiple patterns
        .pattern_len = 0, // Not used for multiple patterns
        .patterns = patterns,
        .pattern_lens = pattern_lens,
        .num_patterns = num_patterns,
        .case_sensitive = case_sensitive,
        .use_regex = false,
        .track_positions = false, // Compatibility wrapper doesn't track positions
        .count_lines_mode = false,
        .compiled_regex = NULL,
        .max_count = SIZE_MAX // Default: no limit for compat wrapper
    };

    // Call the new function signature
    return aho_corasick_search(&params, text, effective_len, NULL);
}

/* memchr_short_search compatibility wrapper */
static inline uint64_t memchr_short_search_compat(
    const char *text, size_t text_len,
    const char *pattern, size_t pattern_len,
    bool case_sensitive, size_t report_limit_offset)
{
    // Apply report_limit_offset by limiting the text length if needed
    size_t effective_len = (report_limit_offset < text_len) ? report_limit_offset : text_len;

    // Create a search_params_t structure with the required parameters
    search_params_t params = {
        .pattern = pattern,
        .pattern_len = pattern_len,
        .case_sensitive = case_sensitive,
        .use_regex = false,
        .track_positions = false,
        .count_lines_mode = false,
        .compiled_regex = NULL};

    // Call the new function signature with the adjusted text length
    return memchr_short_search(&params, text, effective_len, NULL);
}

/*
 * Special functions for test_krep.c that accept search_params_t and extract fields
 * These have completely different names to avoid conflicts with the compat functions
 */

static inline uint64_t test_bridge_boyer_moore(
    const search_params_t *params,
    const char *text, size_t text_len,
    match_result_t *result)
{
    // Call the actual function, passing the max_count from params
    return boyer_moore_search(params, text, text_len, result);
}

static inline uint64_t test_bridge_kmp(
    const search_params_t *params,
    const char *text, size_t text_len,
    match_result_t *result)
{
    // Call the actual function, passing the max_count from params
    return kmp_search(params, text, text_len, result);
}

/* Bridge function for regex_search with params struct */
static inline uint64_t test_bridge_regex(
    const search_params_t *params,
    const char *text, size_t text_len,
    match_result_t *result)
{
    // Call the actual function, passing the max_count from params
    return regex_search(params, text, text_len, result);
}

#endif /* TESTING */

#endif /* TEST_COMPAT_H */