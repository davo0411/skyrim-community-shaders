#include "GlobMatcher.h"

#include <algorithm>
#include <cctype>

namespace Util
{
	std::string ToLower(std::string_view str)
	{
		std::string result;
		result.reserve(str.size());
		for (char c : str) {
			result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
		return result;
	}

	std::string NormalizePath(std::string_view path)
	{
		std::string result = ToLower(path);

		// Convert backslashes to forward slashes
		std::replace(result.begin(), result.end(), '\\', '/');

		// Remove leading "data/" if present
		if (result.starts_with("data/")) {
			result = result.substr(5);
		}

		return result;
	}

	GlobMatcher::GlobMatcher(std::string_view patternView)
		: pattern(patternView)
	{
		patternLower = ToLower(pattern);

		// Normalize path separators in pattern
		std::replace(patternLower.begin(), patternLower.end(), '\\', '/');

		// Analyze pattern type for optimization
		hasWildcards = patternLower.find('*') != std::string::npos ||
		               patternLower.find('?') != std::string::npos;

		if (!hasWildcards) {
			patternType = PatternType::Exact;
			fixedPart = patternLower;
		} else if (patternLower.find('?') != std::string::npos) {
			// ? requires complex matching
			patternType = PatternType::Complex;
		} else {
			// Count asterisks and their positions
			size_t firstStar = patternLower.find('*');
			size_t lastStar = patternLower.rfind('*');

			if (firstStar == lastStar) {
				// Single asterisk
				if (firstStar == 0) {
					// *suffix
					patternType = PatternType::Suffix;
					fixedPart = patternLower.substr(1);
				} else if (firstStar == patternLower.size() - 1) {
					// prefix*
					patternType = PatternType::Prefix;
					fixedPart = patternLower.substr(0, patternLower.size() - 1);
				} else {
					patternType = PatternType::Complex;
				}
			} else if (firstStar == 0 && lastStar == patternLower.size() - 1) {
				// Check if it's *contains* pattern (only stars at start and end)
				std::string_view middle = std::string_view(patternLower).substr(1, patternLower.size() - 2);
				if (middle.find('*') == std::string_view::npos) {
					patternType = PatternType::Contains;
					fixedPart = std::string(middle);
				} else {
					patternType = PatternType::Complex;
				}
			} else {
				patternType = PatternType::Complex;
			}
		}
	}

	bool GlobMatcher::Match(std::string_view text) const
	{
		if (!valid) {
			return false;
		}

		// Normalize the input text
		std::string textLower = ToLower(text);
		std::replace(textLower.begin(), textLower.end(), '\\', '/');

		switch (patternType) {
		case PatternType::Exact:
			return MatchExact(textLower);
		case PatternType::Prefix:
			return MatchPrefix(textLower);
		case PatternType::Suffix:
			return MatchSuffix(textLower);
		case PatternType::Contains:
			return MatchContains(textLower);
		case PatternType::Complex:
		default:
			return MatchComplex(textLower);
		}
	}

	bool GlobMatcher::MatchExact(std::string_view text) const
	{
		return text == fixedPart;
	}

	bool GlobMatcher::MatchPrefix(std::string_view text) const
	{
		return text.starts_with(fixedPart);
	}

	bool GlobMatcher::MatchSuffix(std::string_view text) const
	{
		return text.ends_with(fixedPart);
	}

	bool GlobMatcher::MatchContains(std::string_view text) const
	{
		return text.find(fixedPart) != std::string_view::npos;
	}

	bool GlobMatcher::MatchComplex(std::string_view text) const
	{
		// Use dynamic programming approach for general glob matching
		// This handles multiple wildcards and ? characters

		const size_t m = patternLower.size();
		const size_t n = text.size();

		// dp[i][j] = true if pattern[0..i-1] matches text[0..j-1]
		// Use two rows to save memory
		std::vector<bool> prev(n + 1, false);
		std::vector<bool> curr(n + 1, false);

		prev[0] = true;

		// Handle leading *s
		for (size_t i = 1; i <= m && patternLower[i - 1] == '*'; ++i) {
			prev[0] = true;
		}

		for (size_t i = 1; i <= m; ++i) {
			curr[0] = prev[0] && patternLower[i - 1] == '*';

			for (size_t j = 1; j <= n; ++j) {
				if (patternLower[i - 1] == '*') {
					// * can match empty or consume characters from text
					curr[j] = prev[j] || curr[j - 1];
				} else if (patternLower[i - 1] == '?' || patternLower[i - 1] == text[j - 1]) {
					// ? matches any single character, or exact match
					curr[j] = prev[j - 1];
				} else {
					curr[j] = false;
				}
			}

			std::swap(prev, curr);
		}

		return prev[n];
	}

	bool GlobMatcher::MatchGlob(std::string_view pattern, std::string_view text)
	{
		GlobMatcher matcher(pattern);
		return matcher.Match(text);
	}

	// GlobMatcherSet implementation

	void GlobMatcherSet::AddPattern(std::string_view pattern)
	{
		matchers.emplace_back(pattern);
	}

	void GlobMatcherSet::AddPatterns(const std::vector<std::string>& patterns)
	{
		matchers.reserve(matchers.size() + patterns.size());
		for (const auto& pattern : patterns) {
			matchers.emplace_back(pattern);
		}
	}

	bool GlobMatcherSet::MatchAny(std::string_view text) const
	{
		for (const auto& matcher : matchers) {
			if (matcher.Match(text)) {
				return true;
			}
		}
		return false;
	}

	std::string_view GlobMatcherSet::GetFirstMatch(std::string_view text) const
	{
		for (const auto& matcher : matchers) {
			if (matcher.Match(text)) {
				return matcher.GetPattern();
			}
		}
		return {};
	}
}
