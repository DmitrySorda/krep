/* aho_corasick.c - Memory-optimized implementation of Aho-Corasick algorithm
 *
 * This implementation uses a more memory-efficient representation for trie nodes
 * to reduce the 2KB overhead per node (256 pointers x 8 bytes on 64-bit systems).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h> // Include for tolower

#include "krep.h"         // Include main header FIRST for search_params_t definition
#include "aho_corasick.h" // Include the header defining ac_trie_t forward decl

typedef struct ac_node ac_node_t;

typedef struct
{
    unsigned char ch;
    ac_node_t *node;
} ac_edge_t;

struct ac_node
{
    ac_edge_t *children;           // Sparse child edges for better cache locality
    int num_children;
    int capacity_children;
    ac_node_t *fail_link;          // Failure link for state transitions
    size_t *output_indices;        // Array of pattern indices ending at this node
    int num_outputs;               // Number of pattern indices ending exactly here
    int capacity_outputs;          // Capacity of the output_indices array
};

struct ac_trie
{
    ac_node_t *root;                  // Root node of the trie
    ac_node_t *root_children[256];    // Dense root cache; all other nodes stay sparse
    size_t num_patterns;              // Number of patterns in the trie
    bool case_sensitive;              // Whether search is case-sensitive
};

// Create a new AC node
static ac_node_t *ac_node_create()
{
    ac_node_t *node = calloc(1, sizeof(ac_node_t));
    if (!node)
    {
        perror("Failed to allocate memory for Aho-Corasick node");
        return NULL;
    }
    node->children = NULL;
    node->num_children = 0;
    node->capacity_children = 0;
    node->output_indices = NULL;
    node->num_outputs = 0;
    node->capacity_outputs = 0;
    node->fail_link = NULL;

    return node;
}

static ac_node_t *ac_node_get_child(const ac_node_t *node, unsigned char c)
{
    if (!node)
        return NULL;
    for (int i = 0; i < node->num_children; ++i)
    {
        if (node->children[i].ch == c)
            return node->children[i].node;
    }
    return NULL;
}

static bool ac_node_set_child(ac_node_t *node, unsigned char c, ac_node_t *child)
{
    if (!node || !child)
        return false;

    for (int i = 0; i < node->num_children; ++i)
    {
        if (node->children[i].ch == c)
        {
            node->children[i].node = child;
            return true;
        }
    }

    if (node->num_children >= node->capacity_children)
    {
        int new_capacity = node->capacity_children == 0 ? 4 : node->capacity_children * 2;
        if (new_capacity < node->capacity_children)
            return false;
        ac_edge_t *new_children = realloc(node->children, (size_t)new_capacity * sizeof(ac_edge_t));
        if (!new_children)
        {
            perror("Failed to resize Aho-Corasick child edges");
            return false;
        }
        node->children = new_children;
        node->capacity_children = new_capacity;
    }

    node->children[node->num_children].ch = c;
    node->children[node->num_children].node = child;
    node->num_children++;
    return true;
}

// Add pattern index to node outputs
static bool ac_node_add_output(ac_node_t *node, size_t pattern_index)
{
    if (!node)
        return false;

    // Check if we need to allocate or resize
    if (node->num_outputs >= node->capacity_outputs)
    {
        int new_capacity = node->capacity_outputs == 0 ? 4 : node->capacity_outputs * 2;
        // Check for potential integer overflow before multiplication
        if (new_capacity < node->capacity_outputs)
        {
            perror("Integer overflow calculating new capacity for Aho-Corasick node outputs");
            return false;
        }
        size_t *new_outputs = realloc(node->output_indices, new_capacity * sizeof(size_t));
        if (!new_outputs)
        {
            perror("Failed to resize Aho-Corasick node outputs");
            return false;
        }
        node->output_indices = new_outputs;
        node->capacity_outputs = new_capacity;
    }

    // Add the pattern index
    node->output_indices[node->num_outputs++] = pattern_index;
    return true;
}

// Free an AC node and all its children recursively
static void ac_node_free(ac_node_t *node)
{
    if (!node)
        return;

    // Recursively free children
    for (int i = 0; i < node->num_children; i++)
    {
        if (node->children[i].node)
        {
            ac_node_free(node->children[i].node);
        }
    }

    // Free outputs array if allocated
    if (node->output_indices)
    {
        free(node->output_indices);
    }
    free(node->children);

    // Free the node itself
    free(node);
}

// Build the Aho-Corasick Trie
ac_trie_t *ac_trie_build(const search_params_t *params)
{
    if (!params || params->num_patterns == 0)
    {
        return NULL;
    }

    // Allocate the trie structure
    ac_trie_t *trie = malloc(sizeof(ac_trie_t));
    if (!trie)
    {
        perror("Failed to allocate Aho-Corasick trie");
        return NULL;
    }
    memset(trie->root_children, 0, sizeof(trie->root_children));

    // Create the root node
    trie->root = ac_node_create();
    if (!trie->root)
    {
        free(trie);
        return NULL;
    }
    trie->root->fail_link = trie->root; // Root's failure link points to itself

    trie->num_patterns = params->num_patterns;
    trie->case_sensitive = params->case_sensitive;

    // Build the trie - Insert all patterns
    for (size_t p = 0; p < params->num_patterns; p++)
    {
        const unsigned char *pattern = (const unsigned char *)params->patterns[p];
        size_t pattern_len = params->pattern_lens[p];

        // Handle empty pattern: Add index 0 to root's output if an empty pattern exists
        if (pattern_len == 0)
        {
            if (!ac_node_add_output(trie->root, p))
            {
                ac_trie_free(trie);
                return NULL;
            }
            continue;
        }

        ac_node_t *current = trie->root;

        // Insert each character of the pattern
        for (size_t i = 0; i < pattern_len; i++)
        {
            unsigned char c_orig = pattern[i];
            unsigned char c = params->case_sensitive ? c_orig : lower_table[c_orig];

            // Create child node if it doesn't exist
            ac_node_t *child = ac_node_get_child(current, c);
            if (!child)
            {
                child = ac_node_create();
                if (!child || !ac_node_set_child(current, c, child))
                {
                    // Handle allocation failure
                    ac_node_free(child);
                    ac_trie_free(trie);
                    return NULL;
                }
                if (current == trie->root)
                    trie->root_children[c] = child;
            }

            // Advance to the child
            current = child;
        }

        // Mark this node with the pattern index (used later for output)
        if (!ac_node_add_output(current, p))
        {
            ac_trie_free(trie);
            return NULL;
        }
    }

    // --- Build failure links using BFS with dynamic queue ---
    size_t queue_capacity = 64; // Initial capacity
    ac_node_t **queue = malloc(queue_capacity * sizeof(ac_node_t *));
    if (!queue)
    {
        perror("Failed to allocate initial queue for Aho-Corasick BFS");
        ac_trie_free(trie);
        return NULL;
    }

    size_t queue_front = 0;
    size_t queue_rear = 0;

    // Add root's immediate children to the queue and set their failure links to root
    for (int edge_idx = 0; edge_idx < trie->root->num_children; edge_idx++)
    {
            ac_node_t *child = trie->root->children[edge_idx].node;
            child->fail_link = trie->root; // Direct children fail to root

            // Enqueue
            if (queue_rear >= queue_capacity)
            {
                queue_capacity *= 2;
                ac_node_t **new_queue = realloc(queue, queue_capacity * sizeof(ac_node_t *));
                if (!new_queue)
                {
                    perror("Failed to resize queue for Aho-Corasick BFS");
                    free(queue);
                    ac_trie_free(trie);
                    return NULL;
                }
                queue = new_queue;
            }
            queue[queue_rear++] = child;
    }

    // BFS to build failure links
    while (queue_front < queue_rear)
    {
        ac_node_t *current = queue[queue_front++]; // Dequeue

        // Process each child of current node
        for (int edge_idx = 0; edge_idx < current->num_children; edge_idx++)
        {
                unsigned char c = current->children[edge_idx].ch;
                ac_node_t *child = current->children[edge_idx].node;

                // Enqueue child
                if (queue_rear >= queue_capacity)
                {
                    queue_capacity *= 2;
                    ac_node_t **new_queue = realloc(queue, queue_capacity * sizeof(ac_node_t *));
                    if (!new_queue)
                    {
                        perror("Failed to resize queue during Aho-Corasick BFS");
                        free(queue);
                        ac_trie_free(trie);
                        return NULL;
                    }
                    queue = new_queue;
                }
                queue[queue_rear++] = child;

                // Find failure link for this child
                ac_node_t *failure = current->fail_link;
                while (failure != trie->root && !ac_node_get_child(failure, c))
                {
                    failure = failure->fail_link;
                }

                // Set the failure link
                ac_node_t *failure_child = (failure == trie->root) ? trie->root_children[c] : ac_node_get_child(failure, c);
                child->fail_link = failure_child ? failure_child : trie->root;
        }
    }

    free(queue);
    return trie;
}

// Free the Aho-Corasick Trie
void ac_trie_free(ac_trie_t *trie)
{
    if (!trie)
        return;

    // Free the root node and all its children
    ac_node_free(trie->root);

    // Free the trie structure
    free(trie);
}

// Check if the root node has outputs (used for empty pattern matching in empty files)
bool ac_trie_root_has_outputs(const ac_trie_t *trie)
{
    return (trie != NULL && trie->root != NULL && trie->root->num_outputs > 0);
}

// Forward declarations for helper functions (assuming they exist in krep.c or elsewhere)
extern size_t find_line_start(const char *text, size_t max_len, size_t pos);
extern bool is_whole_word_match(const char *text, size_t text_len, size_t start, size_t end);
// Match the declaration in krep.h (remove const)
extern unsigned char lower_table[256];

// Corrected Aho-Corasick search function
uint64_t aho_corasick_search(const search_params_t *params,
                             const char *text_start,
                             size_t text_len,
                             match_result_t *result)
{
    // --- 1. Validations ---
    // Check for NULL pointers for essential structures
    if (!params || !params->ac_trie || !text_start)
        return 0;
    // Add check for root node existence
    if (!params->ac_trie->root)
    {
        // Trie structure exists but root is NULL, indicates build failure
        fprintf(stderr, "Warning: Aho-Corasick trie root is NULL during search.\n");
        return 0;
    }
    // If max_count is 0, no matches should be found.
    if (params->max_count == 0)
        return 0;

    ac_trie_t *trie = params->ac_trie;
    ac_node_t *current_node = trie->root;
    uint64_t matches_found = 0;
    const size_t max_count = params->max_count; // Use const for clarity
    const bool count_lines_mode = params->count_lines_mode;
    const bool track_positions = params->track_positions;
    size_t last_counted_line_start = SIZE_MAX; // Used only if count_lines_mode is true

    // --- 2. Iterate through the text ---
    for (size_t i = 0; i < text_len; i++)
    {
        // --- 3. Get character and handle case-insensitivity ---
        unsigned char c_orig = (unsigned char)text_start[i];
        // Use lower_table for case-insensitive matching during search
        unsigned char c = params->case_sensitive ? c_orig : lower_table[c_orig];

        // --- 4. Follow failure links ---
        // Traverse failure links until a node with a transition for 'c' is found,
        // or until the root node is reached.
        while (current_node != trie->root && !ac_node_get_child(current_node, c))
        {
            current_node = current_node->fail_link;
        }

        // --- 5. Make state transition ---
        // If a transition for 'c' exists from the current node (or a node reached via failure links),
        // move to that child node. Otherwise, stay at the root.
        ac_node_t *next_node = (current_node == trie->root) ? trie->root_children[c] : ac_node_get_child(current_node, c);
        if (next_node)
        {
            current_node = next_node;
        }
        // If no transition exists even from the root, current_node remains root for the next character.

        // --- 6. Collect matches by following failure links from the current state ---
        ac_node_t *output_node = current_node;
        // Check the current node and all nodes reachable via failure links for outputs.
        // This ensures we find all patterns ending at the current position 'i'.
        while (output_node != trie->root)
        {
            if (output_node->num_outputs > 0)
            {
                // --- 7. Process patterns ending at this output_node ---
                for (int j = 0; j < output_node->num_outputs; j++)
                {
                    // Check max_count *before* processing this specific match.
                    // If we've already reached the limit, return immediately.
                    if (matches_found >= max_count)
                    {
                        return matches_found;
                    }

                    size_t pattern_idx = output_node->output_indices[j];
                    size_t pattern_len = params->pattern_lens[pattern_idx];

                    // Skip empty patterns (should not happen if build logic is correct, but safe check)
                    if (pattern_len == 0)
                        continue;

                    // --- 8. Calculate match position ---
                    // Match ends *at* index i (inclusive).
                    // Start index is i - pattern_len + 1.
                    size_t match_start = i + 1 - pattern_len;
                    size_t match_end = i + 1; // Exclusive end position

                    // --- 9. Apply whole word check ---
                    if (params->whole_word && !is_whole_word_match(text_start, text_len, match_start, match_end))
                    {
                        continue; // Skip if not a whole word match
                    }

                    // --- 10. Process match based on mode ---
                    if (count_lines_mode)
                    {
                        size_t line_start = find_line_start(text_start, text_len, match_start);
                        // Only count if this line hasn't been counted yet for this search
                        if (line_start != last_counted_line_start)
                        {
                            // We already checked max_count at the start of the inner loop
                            matches_found++;
                            last_counted_line_start = line_start;
                            // Check again immediately after incrementing in case this was the last one needed
                            if (matches_found >= max_count)
                                return matches_found;
                        }
                    }
                    else // Count matches or track positions
                    {
                        // We already checked max_count at the start of the inner loop
                        matches_found++;

                        // Add position if tracking is enabled and result struct is provided
                        if (track_positions && result)
                        {
                            match_result_add(result, match_start, match_end);
                        }
                        // Check again immediately after incrementing
                        if (matches_found >= max_count)
                            return matches_found;
                    }
                } // End loop through outputs at this node
            }

            // --- 11. Follow failure link ---
            // Move to the failure link node to find shorter patterns ending at the same position 'i'.
            output_node = output_node->fail_link;

            // Optimization: Check max_count again before continuing the inner while loop.
            // If the limit was reached while processing outputs of the current output_node,
            // we can stop checking further failure links for this text position 'i'.
            if (matches_found >= max_count)
                return matches_found;

        } // End while(output_node != trie->root)

        // Check max_count one last time after processing all outputs for position `i`
        if (matches_found >= max_count)
            return matches_found;

    } // End loop through text (for size_t i = 0; ...)

    // --- Handle potential empty pattern match in empty text ---
    // This case is only relevant if the input text itself was empty.
    // An empty pattern ("") should match an empty text exactly once if present.
    if (text_len == 0 && trie->root->num_outputs > 0)
    {
        for (int j = 0; j < trie->root->num_outputs; ++j)
        {
            size_t pattern_idx = trie->root->output_indices[j];
            // Check if this output corresponds to the empty pattern
            if (params->pattern_lens[pattern_idx] == 0)
            {
                // Found the empty pattern match at the root
                if (matches_found < max_count) // Should always be true if text_len is 0 unless max_count was 0
                {
                    matches_found++;
                    if (track_positions && result)
                    {
                        match_result_add(result, 0, 0); // Empty match at position 0
                    }
                }
                // Only count one match for the empty pattern in empty text
                break;
            }
        }
    }

    return matches_found;
}
