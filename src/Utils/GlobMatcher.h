#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Util
{
	/**
	 * @brief Fast glob pattern matching utility for mesh/texture path matching
	 *
	 * Supports:
	 * - '*' matches any sequence of characters (including empty)
	 * - '?' matches any single character
	 * - Case-insensitive matching
	 *
	 * Does NOT support:
	 * - Character classes [abc]
	 * - Negation [!abc]
	 * - Path separator awareness (treats / and \ the same)
	 */
	class GlobMatcher
	{
	public:
		/**
		 * @brief Construct a matcher from a glob pattern
		 * @param pattern Glob pattern (e.g., "*candle*_d.dds")
		 */
		explicit GlobMatcher(std::string_view pattern);

		/**
		 * @brief Check if a string matches the pattern
		 * @param text Text to match against
		 * @return true if the text matches the pattern
		 */
		bool Match(std::string_view text) const;

		/**
		 * @brief Get the original pattern
		 */
		std::string_view GetPattern() const { return pattern; }

		/**
		 * @brief Check if the pattern is valid
		 */
		bool IsValid() const { return valid; }

		/**
		 * @brief Static convenience method for one-off matching
		 * @param pattern Glob pattern
		 * @param text Text to match
		 * @return true if the text matches the pattern
		 */
		static bool MatchGlob(std::string_view pattern, std::string_view text);

	private:
		std::string pattern;
		std::string patternLower;  // Pre-lowercased for case-insensitive matching
		bool valid = true;
		bool hasWildcards = false;

		// Optimized matching for common cases
		bool MatchExact(std::string_view text) const;
		bool MatchPrefix(std::string_view text) const;   // pattern*
		bool MatchSuffix(std::string_view text) const;   // *pattern
		bool MatchContains(std::string_view text) const; // *pattern*
		bool MatchComplex(std::string_view text) const;  // General case

		enum class PatternType
		{
			Exact,     // No wildcards
			Prefix,    // pattern*
			Suffix,    // *pattern
			Contains,  // *pattern*
			Complex    // Multiple wildcards or ?
		};

		PatternType patternType = PatternType::Complex;
		std::string fixedPart;  // The non-wildcard portion for optimized matching
	};

	/**
	 * @brief Collection of glob patterns with efficient matching
	 *
	 * Useful when checking if a string matches any of several patterns
	 */
	class GlobMatcherSet
	{
	public:
		/**
		 * @brief Add a pattern to the set
		 * @param pattern Glob pattern to add
		 */
		void AddPattern(std::string_view pattern);

		/**
		 * @brief Add multiple patterns from a vector
		 * @param patterns Vector of glob patterns
		 */
		void AddPatterns(const std::vector<std::string>& patterns);

		/**
		 * @brief Check if any pattern in the set matches the text
		 * @param text Text to match
		 * @return true if any pattern matches
		 */
		bool MatchAny(std::string_view text) const;

		/**
		 * @brief Get the first matching pattern, if any
		 * @param text Text to match
		 * @return The matching pattern, or empty string if none match
		 */
		std::string_view GetFirstMatch(std::string_view text) const;

		/**
		 * @brief Check if the set is empty
		 */
		bool IsEmpty() const { return matchers.empty(); }

		/**
		 * @brief Get the number of patterns
		 */
		size_t Size() const { return matchers.size(); }

		/**
		 * @brief Clear all patterns
		 */
		void Clear() { matchers.clear(); }

	private:
		std::vector<GlobMatcher> matchers;
	};

	/**
	 * @brief Normalize a path for consistent matching
	 *
	 * - Converts backslashes to forward slashes
	 * - Converts to lowercase
	 * - Removes leading "data/" or "data\" if present
	 *
	 * @param path Path to normalize
	 * @return Normalized path
	 */
	std::string NormalizePath(std::string_view path);

	/**
	 * @brief Convert a string to lowercase
	 * @param str String to convert
	 * @return Lowercase string
	 */
	std::string ToLower(std::string_view str);
}
